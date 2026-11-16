#include "platform/splice/SpliceAuth.h"

#include "platform/HttpClient.h"

namespace sonora::platform::splice
{

namespace
{
std::string str(const juce::var& v) { return v.isVoid() ? std::string() : v.toString().toStdString(); }

HttpResponse postJson(const juce::String& url, const juce::String& userAgent, const juce::var& body,
                      const std::atomic<bool>& cancelled)
{
    HttpRequest req;
    req.url = url;
    req.method = "POST";
    req.headers.set("Content-Type", "application/json");
    req.headers.set("User-Agent", userAgent); // Splice's Cloudflare blocks the default library UA
    req.body = juce::JSON::toString(body, true);
    req.maxResponseBytes = 256 * 1024;
    return postNoRedirect(req, cancelled);
}
} // namespace

DeviceCodeStart SpliceAuth::parseDeviceCode(const juce::String& json)
{
    DeviceCodeStart out;
    const auto v = juce::JSON::parse(json);
    if (! v.isObject()) { out.error = "bad response"; return out; }
    out.deviceCode = str(v.getProperty("device_code", {}));
    out.userCode = str(v.getProperty("user_code", {}));
    out.verificationUri = str(v.getProperty("verification_uri", {}));
    out.verificationUriComplete = str(v.getProperty("verification_uri_complete", {}));
    out.intervalSeconds = (int) v.getProperty("interval", 5);
    out.expiresInSeconds = (int) v.getProperty("expires_in", 900);
    if (out.deviceCode.empty()) { out.error = str(v.getProperty("error_description", {})); if (out.error.empty()) out.error = "no device code"; }
    else out.ok = true;
    return out;
}

TokenSet SpliceAuth::parseToken(const juce::String& json, int status)
{
    TokenSet out;
    const auto v = juce::JSON::parse(json);
    const auto err = str(v.getProperty("error", {}));
    if (! err.empty())
    {
        // Device-code flow: still waiting for the user, or slow down the poll.
        out.error = (err == "authorization_pending" || err == "slow_down") ? "pending" : err;
        return out;
    }
    out.accessToken = str(v.getProperty("access_token", {}));
    out.refreshToken = str(v.getProperty("refresh_token", {}));
    const int64_t expiresIn = (int64_t) v.getProperty("expires_in", 3600);
    out.expiresAtMs = juce::Time::currentTimeMillis() + expiresIn * 1000;
    if (out.accessToken.empty()) { out.error = status >= 400 ? ("http " + std::to_string(status)) : "no token"; return out; }
    out.ok = true;
    return out;
}

DeviceCodeStart SpliceAuth::startDeviceCode(const std::atomic<bool>& cancelled) const
{
    auto* body = new juce::DynamicObject();
    body->setProperty("client_id", config_.clientId);
    body->setProperty("audience", config_.audience);
    body->setProperty("scope", config_.scope);
    const auto res = postJson("https://" + config_.domain + "/oauth/device/code", config_.userAgent, juce::var(body), cancelled);
    if (res.outcome != HttpResponse::Outcome::Completed) { DeviceCodeStart d; d.error = "network"; return d; }
    return parseDeviceCode(res.body);
}

TokenSet SpliceAuth::pollDeviceToken(const std::string& deviceCode, const std::atomic<bool>& cancelled) const
{
    auto* body = new juce::DynamicObject();
    body->setProperty("grant_type", "urn:ietf:params:oauth:grant-type:device_code");
    body->setProperty("client_id", config_.clientId);
    body->setProperty("device_code", juce::String(deviceCode));
    const auto res = postJson("https://" + config_.domain + "/oauth/token", config_.userAgent, juce::var(body), cancelled);
    if (res.outcome != HttpResponse::Outcome::Completed) { TokenSet t; t.error = "network"; return t; }
    return parseToken(res.body, res.status);
}

TokenSet SpliceAuth::refresh(const std::string& refreshToken, const std::atomic<bool>& cancelled) const
{
    auto* body = new juce::DynamicObject();
    body->setProperty("grant_type", "refresh_token");
    body->setProperty("client_id", config_.clientId);
    body->setProperty("refresh_token", juce::String(refreshToken));
    const auto res = postJson("https://" + config_.domain + "/oauth/token", config_.userAgent, juce::var(body), cancelled);
    if (res.outcome != HttpResponse::Outcome::Completed) { TokenSet t; t.error = "network"; return t; }
    auto out = parseToken(res.body, res.status);
    if (out.ok && out.refreshToken.empty()) out.refreshToken = refreshToken; // keep the old one if not rotated
    return out;
}

} // namespace sonora::platform::splice
