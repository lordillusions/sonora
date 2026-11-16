#include "platform/splice/SpliceStore.h"

#include "platform/HttpClient.h"
#include "platform/SecretStore.h"
#include "platform/splice/SpliceDescramble.h"

namespace sonora::platform::splice
{

namespace
{
SecretId refreshSecret() { return {"Sonora: Splice", "auth0-refresh"}; }
constexpr int64_t kExpiryMarginMs = 60'000; // refresh a minute early
} // namespace

SpliceStore::SpliceStore(SpliceEndpoints endpoints, Auth0Config auth, SecretStore& secrets)
    : endpoints_(std::move(endpoints)), api_(endpoints_), auth_(std::move(auth)), secrets_(secrets)
{
}

std::string SpliceStore::loadRefresh() const
{
    juce::String value;
    if (secrets_.read(refreshSecret(), value).ok()) return value.toStdString();
    return {};
}

void SpliceStore::storeRefresh(const std::string& token)
{
    if (token.empty()) secrets_.remove(refreshSecret());
    else secrets_.write(refreshSecret(), juce::String(token));
}

bool SpliceStore::loggedIn() const
{
    std::lock_guard lock(mutex_);
    if (! loaded_) return secrets_.contains(refreshSecret());
    return ! refreshToken_.empty();
}

std::string SpliceStore::accessToken(const std::atomic<bool>& cancelled)
{
    std::lock_guard lock(mutex_);
    if (! loaded_) { refreshToken_ = loadRefresh(); loaded_ = true; }
    if (refreshToken_.empty()) { needsLogin_ = true; return {}; }
    if (! accessToken_.empty() && juce::Time::currentTimeMillis() < accessExpiresAtMs_ - kExpiryMarginMs)
        return accessToken_;
    const auto tokens = auth_.refresh(refreshToken_, cancelled);
    if (! tokens.ok)
    {
        if (tokens.error != "network") // a real auth failure: forget the login; a network blip keeps it
        {
            refreshToken_.clear();
            accessToken_.clear();
            storeRefresh({});
            needsLogin_ = true;
        }
        return {};
    }
    accessToken_ = tokens.accessToken;
    accessExpiresAtMs_ = tokens.expiresAtMs;
    if (tokens.refreshToken != refreshToken_) { refreshToken_ = tokens.refreshToken; storeRefresh(refreshToken_); }
    return accessToken_;
}

void SpliceStore::invalidateAccessToken()
{
    std::lock_guard lock(mutex_);
    accessToken_.clear();
    accessExpiresAtMs_ = 0;
}

void SpliceStore::signOut()
{
    std::lock_guard lock(mutex_);
    refreshToken_.clear();
    accessToken_.clear();
    accessExpiresAtMs_ = 0;
    loaded_ = true;
    storeRefresh({});
}

DeviceCodeStart SpliceStore::startLogin(const std::atomic<bool>& cancelled)
{
    return auth_.startDeviceCode(cancelled);
}

TokenSet SpliceStore::pollLogin(const std::string& deviceCode, const std::atomic<bool>& cancelled)
{
    auto tokens = auth_.pollDeviceToken(deviceCode, cancelled);
    if (tokens.ok)
    {
        std::lock_guard lock(mutex_);
        accessToken_ = tokens.accessToken;
        accessExpiresAtMs_ = tokens.expiresAtMs;
        refreshToken_ = tokens.refreshToken;
        loaded_ = true;
        storeRefresh(refreshToken_);
        needsLogin_ = false;
    }
    return tokens;
}

std::vector<uint8_t> SpliceStore::fetchPreview(const std::string& url, const std::atomic<bool>& cancelled) const
{
    auto temp = juce::File::createTempFile(".splice-preview");
    DownloadRequest req;
    req.url = juce::String(url);
    req.destination = temp;
    req.maxBytes = 16 * 1024 * 1024;
    const auto result = downloadToFile(req, [](int64_t, int64_t) {}, cancelled);
    std::vector<uint8_t> out;
    if (result.outcome == HttpResponse::Outcome::Completed && result.status >= 200 && result.status < 300)
    {
        juce::MemoryBlock block;
        if (temp.loadFileAsData(block))
            out = descramblePreview(static_cast<const uint8_t*>(block.getData()), block.getSize());
    }
    temp.deleteFile();
    return out;
}

bool SpliceStore::downloadWav(const std::string& url, const juce::File& dest, const std::atomic<bool>& cancelled) const
{
    dest.getParentDirectory().createDirectory();
    if (dest.existsAsFile()) dest.deleteFile(); // content-addressed; a stale partial would resume wrongly
    DownloadRequest req;
    req.url = juce::String(url);
    req.destination = dest;
    req.maxBytes = int64_t(512) << 20;
    const auto result = downloadToFile(req, [](int64_t, int64_t) {}, cancelled);
    return result.outcome == HttpResponse::Outcome::Completed && result.status >= 200 && result.status < 300
        && dest.getSize() > 0;
}

} // namespace sonora::platform::splice
