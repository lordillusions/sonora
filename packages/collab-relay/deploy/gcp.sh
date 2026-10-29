#!/usr/bin/env bash
# COL-3b: run the collab server (relay + Caddy + coturn, docker-compose.yml) on one Google Cloud e2-micro VM.
#
#   deploy/gcp.sh ip [--project P]       reserve the static IP first and print it (for the domain's A record)
#   deploy/gcp.sh create [--project P] [--zone us-east1-b] [--domain relay.sonoradaw.com]
#   deploy/gcp.sh update [--domain D]    copy this folder's relay to the VM again and rebuild
#   deploy/gcp.sh token "Your Name"      create a host account; prints the access key once
#   deploy/gcp.sh status | logs | ssh | delete
#
# Needs the gcloud CLI, signed in (gcloud auth login) with billing enabled on the project. Without --domain the relay
# is reachable at <ip-with-dashes>.sslip.io, which needs no DNS setup. With --domain, create waits until the domain's
# A record points at the static IP, so Caddy can get the certificate. Settings: SONORA_GCP_NAME (default sonora-relay).
set -euo pipefail

name="${SONORA_GCP_NAME:-sonora-relay}"
zone="us-east1-b"
project=""
domain=""
here="$(cd "$(dirname "$0")" && pwd)"
relay_dir="$(cd "$here/.." && pwd)"

command="${1:-help}"
[[ $# -gt 0 ]] && shift
args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --project) project="$2"; shift 2 ;;
        --zone) zone="$2"; shift 2 ;;
        --domain) domain="$2"; shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
region="${zone%-*}"
case "$command" in ip|create|update|token|status|logs|ssh|delete) ;; *) sed -n '2,10p' "$0"; exit 0 ;; esac

command -v gcloud > /dev/null || { echo "Install the Google Cloud CLI first: https://cloud.google.com/sdk/docs/install" >&2; exit 1; }
[[ -n "$project" ]] || project="$(gcloud config get-value project 2> /dev/null || true)"
[[ -n "$project" ]] || { echo "No project: pass --project <id> or run: gcloud config set project <id>" >&2; exit 1; }
g() { gcloud --project "$project" "$@"; }
on_vm() { g compute ssh "$name" --zone "$zone" --quiet --command "$1"; }

public_ip() { g compute addresses describe "$name" --region "$region" --format='value(address)'; }

upload()
{
    local ip="$1" host="$2" archive
    archive="$(mktemp -t sonora-relay.XXXXXX)"
    COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs -C "$relay_dir" --exclude node_modules --exclude 'accounts.json*' --exclude '*.lock' \
        --exclude .env --exclude data -czf "$archive" .
    g compute scp --zone "$zone" --quiet "$archive" "$name:/tmp/sonora-relay.tgz"
    rm -f "$archive"
    # .env is written once and kept: the TURN secret must not change under running sessions.
    on_vm "set -e
        sudo mkdir -p /opt/sonora-relay && sudo tar -C /opt/sonora-relay -xzf /tmp/sonora-relay.tgz && rm /tmp/sonora-relay.tgz
        cd /opt/sonora-relay/deploy
        if [ ! -f .env ]; then
            private=\$(curl -s -H 'Metadata-Flavor: Google' http://metadata.google.internal/computeMetadata/v1/instance/network-interfaces/0/ip)
            printf 'SONORA_RELAY_DOMAIN=%s\nSONORA_PUBLIC_IP=%s\nSONORA_PRIVATE_IP=%s\nSONORA_TURN_SECRET=%s\n' \
                '$host' '$ip' \"\$private\" \"\$(openssl rand -hex 32)\" | sudo tee .env > /dev/null
            sudo chmod 600 .env
        fi
        sudo sed -i 's|^SONORA_RELAY_DOMAIN=.*|SONORA_RELAY_DOMAIN=$host|' .env
        sudo mkdir -p data && sudo chown 1000:1000 data && sudo chmod 700 data
        sudo docker compose up -d --build --remove-orphans"
}

# Docker from Ubuntu's own packages. Mirrors fail now and then (2026-10-22: "Some index files failed to download"
# left the first VM without Docker), so retry, and stop here if it still isn't there.
install_docker()
{
    echo "Installing Docker on the VM..."
    on_vm "for i in 1 2 3 4 5; do
            command -v docker > /dev/null && docker compose version > /dev/null 2>&1 && exit 0
            sudo apt-get -o DPkg::Lock::Timeout=300 update -q || true
            sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 install -y -q docker.io docker-compose-v2 \
                && sudo systemctl enable --now docker
            sleep 15
        done
        docker compose version" > /dev/null || { echo "Docker could not be installed on the VM: run '$0 ssh' and check apt." >&2; exit 1; }
}

reserve_ip()
{
    g services enable compute.googleapis.com
    if ! g compute addresses describe "$name" --region "$region" > /dev/null 2>&1; then
        g compute addresses create "$name" --region "$region" --network-tier STANDARD
    fi
}

case "$command" in
ip)
    reserve_ip
    echo "Static IP: $(public_ip)   (point the relay's A record at it, then run: $0 create --domain <name>)"
    ;;
create)
    reserve_ip
    ip="$(public_ip)"
    [[ -n "$domain" ]] || domain="${ip//./-}.sslip.io"
    if [[ "$domain" != *.sslip.io ]]; then
        echo "Waiting for $domain to point at $ip..."
        for _ in $(seq 1 60); do
            [[ "$(dig +short A "$domain" @8.8.8.8 | tail -1)" == "$ip" ]] && break
            sleep 10
        done
        [[ "$(dig +short A "$domain" @8.8.8.8 | tail -1)" == "$ip" ]] \
            || { echo "$domain does not resolve to $ip yet: create the A record, then run create again." >&2; exit 1; }
    fi
    if ! g compute firewall-rules describe "$name" > /dev/null 2>&1; then
        g compute firewall-rules create "$name" --network default --direction INGRESS --target-tags "$name" \
            --allow tcp:80,tcp:443,tcp:3478,udp:3478,udp:49160-49200
    fi
    if ! g compute instances describe "$name" --zone "$zone" > /dev/null 2>&1; then
        startup="$(mktemp -t sonora-startup.XXXXXX)"
        cat > "$startup" << 'EOF'
#!/bin/bash
# 1 GB of swap for the 1 GB e2-micro. (Docker is installed over SSH by gcp.sh, with retries.)
if [ ! -f /swapfile ]; then
    fallocate -l 1G /swapfile && chmod 600 /swapfile && mkswap /swapfile && swapon /swapfile
    echo '/swapfile none swap sw 0 0' >> /etc/fstab
fi
EOF
        g compute instances create "$name" --zone "$zone" --machine-type e2-micro \
            --image-family ubuntu-2404-lts-amd64 --image-project ubuntu-os-cloud \
            --boot-disk-size 30GB --boot-disk-type pd-standard \
            --address "$ip" --network-tier STANDARD --tags "$name" \
            --metadata-from-file startup-script="$startup"
        rm -f "$startup"
    fi
    echo "Waiting for SSH..."
    for _ in $(seq 1 30); do
        on_vm "true" > /dev/null 2>&1 && break
        sleep 10
    done
    install_docker
    upload "$ip" "$domain"
    echo "Waiting for the HTTPS certificate..."
    for _ in $(seq 1 30); do
        curl -fsS "https://$domain/v1/health" > /dev/null 2>&1 && break
        sleep 5
    done
    curl -fsS "https://$domain/v1/health" > /dev/null || echo "The relay does not answer on https://$domain yet: run '$0 logs'." >&2
    echo
    echo "Relay: https://$domain"
    echo "Next: $0 token \"Your Name\", then in Sonora: Collaborate > Relay: Change... > address + access key."
    ;;
update)
    install_docker
    ip="$(public_ip)"
    host="$domain"
    [[ -n "$host" ]] || host="$(on_vm "sudo grep '^SONORA_RELAY_DOMAIN=' /opt/sonora-relay/deploy/.env | cut -d= -f2")"
    upload "$ip" "${host:-${ip//./-}.sslip.io}"
    ;;
token)
    [[ ${#args[@]} -ge 1 ]] || { echo "usage: $0 token \"Name\"" >&2; exit 1; }
    on_vm "cd /opt/sonora-relay/deploy && sudo docker compose run --rm -T relay node admin.mjs add --plan selfhost --name '${args[0]//\'/}'"
    ;;
status) on_vm "cd /opt/sonora-relay/deploy && sudo docker compose ps && sudo grep '^SONORA_RELAY_DOMAIN=' .env" ;;
logs) on_vm "cd /opt/sonora-relay/deploy && sudo docker compose logs --tail 100" ;;
ssh) g compute ssh "$name" --zone "$zone" ;;
delete)
    g compute instances delete "$name" --zone "$zone" --quiet || true
    g compute firewall-rules delete "$name" --quiet || true
    g compute addresses delete "$name" --region "$region" --quiet || true
    ;;
*)
    sed -n '2,10p' "$0"
    ;;
esac
