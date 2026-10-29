# COL-3b: container image of the collab relay (deploy/docker-compose.yml). TLS is terminated by Caddy in front of it.
FROM node:22-alpine
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev && npm cache clean --force
COPY *.mjs ./
RUN mkdir -p /data && chown node:node /data
USER node
ENV HOST=0.0.0.0 PORT=8787 SONORA_RELAY_ACCOUNTS=/data/accounts.json SONORA_RELAY_TRUST_PROXY=1
EXPOSE 8787
CMD ["node", "relay.mjs"]
