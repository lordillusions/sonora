// SPL-1: Splice client logic — descramble, key math, JSON parsing. No network, no Splice bytes (synthetic vectors only).
#include "TestFramework.h"

#include "platform/splice/SpliceApi.h"
#include "platform/splice/SpliceAuth.h"
#include "platform/splice/SpliceDescramble.h"
#include "platform/splice/SpliceKey.h"

#include <numeric>
#include <string>
#include <vector>

using namespace sonora::platform::splice;

namespace
{
// Build a scrambled preview the way docs/SPLICE_INTEROP.md describes, so descramble must return `plain`.
std::vector<uint8_t> scramble(const std::vector<uint8_t>& plain, const std::string& key18)
{
    std::vector<uint8_t> out;
    out.push_back('s'); out.push_back('p');
    const uint32_t n = (uint32_t) plain.size();
    out.push_back((uint8_t) (n & 0xff)); out.push_back((uint8_t) ((n >> 8) & 0xff));
    out.push_back((uint8_t) ((n >> 16) & 0xff)); out.push_back((uint8_t) ((n >> 24) & 0xff));
    out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0); // bytes 6..9
    for (char c : key18) out.push_back((uint8_t) c); // bytes 10..27
    std::vector<uint8_t> body = plain;
    const size_t q = body.size() / 4;
    for (size_t block : {size_t(0), size_t(2)})
        for (size_t j = 0; j < q; ++j)
            body[block * q + j] ^= (uint8_t) key18[j % 18];
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
} // namespace

TEST_CASE("splice: descramble round-trips and leaves clear quarters untouched")
{
    std::vector<uint8_t> plain(1000);
    std::iota(plain.begin(), plain.end(), 0); // 0,1,2,... mod 256
    const std::string key = "bee7d8d9-0d57-be"; // 16 chars... pad to 18
    const std::string key18 = key + "80";
    CHECK(key18.size() == 18);
    auto scrambled = scramble(plain, key18);
    CHECK(isScrambledPreview(scrambled.data(), scrambled.size()));
    auto back = descramblePreview(scrambled);
    CHECK(back == plain);

    // The 2nd and 4th quarters of the scrambled body equal the plain bytes (not masked).
    const size_t q = plain.size() / 4;
    CHECK(scrambled[28 + q] == plain[q]);           // start of 2nd quarter
    CHECK(scrambled[28 + 3 * q] == plain[3 * q]);    // start of 4th quarter
    CHECK(scrambled[28 + 0] != plain[0] || plain[0] == (plain[0] ^ (uint8_t) key18[0])); // 1st quarter masked
}

TEST_CASE("splice: a plain mp3 without the header is returned unchanged")
{
    std::vector<uint8_t> mp3{0xff, 0xfb, 0x90, 0x44, 1, 2, 3, 4};
    CHECK(! isScrambledPreview(mp3.data(), mp3.size()));
    CHECK(descramblePreview(mp3) == mp3);
}

TEST_CASE("splice: key parsing")
{
    auto a = parseSpliceKey("a", "minor");
    CHECK(a.valid); CHECK(a.pitchClass == 9); CHECK(a.mode == KeyMode::Minor);
    auto cs = parseSpliceKey("c#", "major");
    CHECK(cs.pitchClass == 1); CHECK(cs.mode == KeyMode::Major);
    auto db = parseSpliceKey("Db", "");
    CHECK(db.pitchClass == 1); CHECK(db.mode == KeyMode::None);
    CHECK(! parseSpliceKey("", "minor").valid);
    CHECK(! parseSpliceKey("h", "").valid);
}

TEST_CASE("splice: relative key, smallest shift, +/-6 bound")
{
    const MusicalKey cMajor{0, KeyMode::Major, true};
    // A minor is the relative minor of C major -> no transpose.
    CHECK(transposeToProject(parseSpliceKey("a", "minor"), cMajor) == 0);
    // C major sample into C major -> 0.
    CHECK(transposeToProject(parseSpliceKey("c", "major"), cMajor) == 0);
    // D major into C major -> down 2.
    CHECK(transposeToProject(parseSpliceKey("d", "major"), cMajor) == -2);
    // C minor into C major: relative major of C minor is Eb(3); fold(0-3) = -3.
    CHECK(transposeToProject(parseSpliceKey("c", "minor"), cMajor) == -3);
    // F# (6) into C: ties fold downward to -6, never beyond.
    const int t = transposeToProject(parseSpliceKey("f#", "major"), cMajor);
    CHECK(t >= -6); CHECK(t <= 6); CHECK(t == 6 || t == -6);
    // No mode on the sample -> tonic to tonic. E (4) into C major -> fold(0-4) = -4.
    CHECK(transposeToProject(parseSpliceKey("e", ""), cMajor) == -4);
    // Invalid sample -> 0 (nothing known to match).
    CHECK(transposeToProject(parseSpliceKey("", ""), cMajor) == 0);
}

TEST_CASE("splice: projectKey classifies major/minor scales")
{
    CHECK(projectKey(0, 0).mode == KeyMode::Major); // C Major
    CHECK(projectKey(9, 1).mode == KeyMode::Minor); // A Minor
    CHECK(projectKey(9, 1).pitchClass == 9);
}

TEST_CASE("splice: parse search JSON")
{
    const char* json = R"({"assetsSearch":{"items":[
      {"uuid":"u1","name":"Pack/kick.wav","licensed":false,"bpm":128,"duration":2000,"key":"a","chord_type":"minor","asset_category_slug":"oneshot",
       "asset_prices":[{"amount":1,"currency":"Credits"}],"tags":[{"label":"drums"}],
       "files":[{"asset_file_type_slug":"preview_mp3","url":"https://s3/x.mp3"}],
       "parents":{"items":[{"uuid":"p1","name":"Pack","provider":{"name":"ModeAudio"},"files":[{"asset_file_type_slug":"cover_image","url":"https://img/c.png"}]}]}}
    ],"pagination_metadata":{"currentPage":1,"totalPages":8},"response_metadata":{"records":200}}})";
    auto r = SpliceApi::parseSearch(json);
    CHECK(r.ok); REQUIRE(r.samples.size() == 1);
    const auto& s = r.samples[0];
    CHECK(s.uuid == "u1"); CHECK((int) s.bpm == 128); CHECK(s.key == "a"); CHECK(s.chordType == "minor");
    CHECK(s.priceCredits == 1); CHECK(! s.licensed); CHECK(! s.isLoop);
    CHECK(s.previewUrl == "https://s3/x.mp3"); CHECK(s.packName == "Pack"); CHECK(s.providerName == "ModeAudio");
    CHECK(s.coverUrl == "https://img/c.png"); CHECK(r.totalPages == 8); CHECK(r.totalRecords == 200);
    REQUIRE(s.tags.size() == 1); CHECK(s.tags[0] == "drums");
}

TEST_CASE("splice: parse cost / purchase / premium JSON")
{
    auto cost = SpliceApi::parseCost(R"({"data":{"costEstimate":{"cost":2,"userCredits":680}}})");
    CHECK(cost.ok); CHECK(cost.cost == 2); CHECK(cost.userCredits == 680);

    auto hash = SpliceApi::parsePurchase(
        R"({"data":{"purchaseAssets":[{"assetUuid":"u1","purchased":true,"asset":{"uuid":"deadbeef","licensed":true}}]}})", "u1");
    CHECK(hash == "deadbeef");
    // Already owned (purchased false but licensed true) still returns the hash.
    auto owned = SpliceApi::parsePurchase(
        R"({"data":{"purchaseAssets":[{"assetUuid":"u1","purchased":false,"asset":{"uuid":"h2","licensed":true}}]}})", "u1");
    CHECK(owned == "h2");

    auto dl = SpliceApi::parsePremium(R"({"sample":{"url":"https://s3/real.wav","path":"packs/a/b.wav","file_size":2595470}})");
    CHECK(dl.ok); CHECK(dl.url == "https://s3/real.wav"); CHECK(dl.path == "packs/a/b.wav"); CHECK(dl.size == 2595470);
}

// Manual live check (never in CI): set SONORA_SPLICE_LIVE_TOKEN to a real Bearer access token to hit the server.
TEST_CASE("splice: live search and cost (manual)")
{
    const auto token = juce::SystemStats::getEnvironmentVariable("SONORA_SPLICE_LIVE_TOKEN", {}).toStdString();
    if (token.empty()) return; // skipped by default
    SpliceApi api;
    std::atomic<bool> cancel{false};
    SpliceSearchQuery q; q.text = "piano loop"; q.limit = 3;
    const auto r = api.searchSamples(token, q, cancel);
    std::printf("[live] search ok=%d n=%zu total=%d err=%s\n", r.ok, r.samples.size(), r.totalRecords, r.error.c_str());
    CHECK(r.ok);
    REQUIRE(! r.samples.empty());
    std::printf("[live] first: %s bpm=%.0f key=%s price=%d licensed=%d\n", r.samples[0].name.c_str(), r.samples[0].bpm,
               r.samples[0].key.c_str(), r.samples[0].priceCredits, r.samples[0].licensed);
    CHECK(! r.samples[0].uuid.empty());
    // Search is public, so it proves the network path even with a stale token. Cost/balance need a real token; only
    // check them when one that parses as a signed JWT is supplied (SONORA_SPLICE_LIVE_AUTHED=1).
    const bool authed = juce::SystemStats::getEnvironmentVariable("SONORA_SPLICE_LIVE_AUTHED", {}) == "1";
    const auto cost = api.costEstimate(token, {r.samples[0].uuid}, cancel);
    std::printf("[live] cost=%d credits=%d ok=%d\n", cost.cost, cost.userCredits, cost.ok);
    const int bal = api.creditBalance(token, cancel);
    std::printf("[live] balance=%d\n", bal);
    if (authed) { CHECK(cost.ok); CHECK(bal >= 0); }
}

TEST_CASE("splice auth: parse device code and token")
{
    auto d = SpliceAuth::parseDeviceCode(
        R"({"device_code":"dc","user_code":"JNHJ-KQBS","verification_uri":"https://auth.splice.com/activate","verification_uri_complete":"https://auth.splice.com/activate?user_code=JNHJ-KQBS","interval":5,"expires_in":900})");
    CHECK(d.ok); CHECK(d.userCode == "JNHJ-KQBS"); CHECK(d.intervalSeconds == 5);
    CHECK(d.verificationUriComplete.find("JNHJ-KQBS") != std::string::npos);

    auto pending = SpliceAuth::parseToken(R"({"error":"authorization_pending","error_description":"..."})", 403);
    CHECK(! pending.ok); CHECK(pending.error == "pending");

    auto ok = SpliceAuth::parseToken(R"({"access_token":"AAA","refresh_token":"RRR","expires_in":3600,"token_type":"Bearer"})", 200);
    CHECK(ok.ok); CHECK(ok.accessToken == "AAA"); CHECK(ok.refreshToken == "RRR"); CHECK(ok.expiresAtMs > 0);
}

TEST_CASE("splice: auth errors are told apart from other failures (SPL-12)")
{
    // 401 or a GraphQL auth error means "sign in again"; Cloudflare's 403 (UA block), 5xx and ordinary GraphQL errors do not.
    CHECK(SpliceApi::isAuthError(401, {}));
    CHECK(SpliceApi::isAuthError(200, "UNAUTHENTICATED"));
    CHECK(SpliceApi::isAuthError(200, "Unauthorized"));
    CHECK(SpliceApi::isAuthError(200, "jwt expired"));
    CHECK(! SpliceApi::isAuthError(403, {}));
    CHECK(! SpliceApi::isAuthError(500, {}));
    CHECK(! SpliceApi::isAuthError(200, "Asset not found"));
    SpliceApi api;
    CHECK(! api.takeUnauthorized());
}
