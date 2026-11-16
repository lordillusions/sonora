#include "platform/splice/SpliceApi.h"

#include "platform/HttpClient.h"

namespace sonora::platform::splice
{

namespace
{
// GraphQL documents (docs/SPLICE_INTEROP.md). These are query strings sent to Splice's public endpoint, not Splice code.
constexpr const char* kSearchQuery = R"(query SamplesSearch($query: String, $page: Int, $limit: Int = 50, $sort: AssetSortType = relevance, $order: SortOrder = DESC, $key: String, $chord_type: String, $min_bpm: Int, $max_bpm: Int, $asset_category_slug: AssetCategorySlug, $semantic: Float = 0) {
  assetsSearch(filter: {published: true, asset_type_slug: sample, query: $query, key: $key, chord_type: $chord_type, min_bpm: $min_bpm, max_bpm: $max_bpm, asset_category_slug: $asset_category_slug, semantic: $semantic}, pagination: {page: $page, limit: $limit}, sort: {sort: $sort, order: $order}) {
    items {
      ... on IAsset { uuid name licensed asset_prices { amount currency } tags { label } files { asset_file_type_slug url } }
      ... on SampleAsset { bpm chord_type duration key asset_category_slug }
      ... on IAssetChild { parents(filter: {asset_type_slug: pack}) { items { ... on PackAsset { uuid name provider { name } files { asset_file_type_slug url } } } } }
    }
    pagination_metadata { currentPage totalPages }
    response_metadata { records }
  }
})";

constexpr const char* kCostQuery =
    R"(query AssetsCostEstimate($assetUuids: [GUID!]!) { costEstimate(assetUuids: $assetUuids) { cost userCredits } })";

constexpr const char* kBalanceQuery = R"(query CreditsStoreQuery { creditBalance })";

constexpr const char* kPurchaseMutation =
    R"(mutation AssetSharedPurchaseAssets($uuids: [GUID!]!, $legacy: Boolean = true) {
  purchaseAssets(uuids: $uuids, legacy: $legacy) { assetUuid purchased asset { ... on IAsset { uuid licensed } } }
})";

std::string str(const juce::var& v) { return v.isVoid() ? std::string() : v.toString().toStdString(); }

juce::var firstPack(const juce::var& item)
{
    const auto parents = item.getProperty("parents", {});
    const auto items = parents.getProperty("items", {});
    if (auto* arr = items.getArray(); arr != nullptr && ! arr->isEmpty()) return (*arr)[0];
    return {};
}

std::string fileUrl(const juce::var& files, const char* slug)
{
    if (auto* arr = files.getArray())
        for (const auto& f : *arr)
            if (str(f.getProperty("asset_file_type_slug", {})) == slug) return str(f.getProperty("url", {}));
    return {};
}
} // namespace

juce::var SpliceApi::graphql(const std::string& token, const char* operationName, const char* query,
                             const juce::var& variables, const std::atomic<bool>& cancelled, std::string& error) const
{
    auto* payload = new juce::DynamicObject();
    payload->setProperty("operationName", juce::String(operationName));
    payload->setProperty("query", juce::String::fromUTF8(query));
    payload->setProperty("variables", variables);

    HttpRequest req;
    req.url = endpoints_.graphql;
    req.method = "POST";
    req.headers.set("Content-Type", "application/json");
    req.headers.set("User-Agent", endpoints_.userAgent);
    req.headers.set("Authorization", "Bearer " + juce::String(token));
    req.body = juce::JSON::toString(juce::var(payload), true);
    req.maxResponseBytes = 8 * 1024 * 1024;

    const auto res = postNoRedirect(req, cancelled);
    if (res.outcome != HttpResponse::Outcome::Completed) { error = "network"; return {}; }
    if (res.status < 200 || res.status >= 300)
    {
        error = "http " + std::to_string(res.status);
        if (isAuthError(res.status, {})) unauthorized_ = true;
        return {};
    }
    const auto parsed = juce::JSON::parse(res.body);
    const auto errors = parsed.getProperty("errors", {});
    if (errors.isArray() && ! errors.getArray()->isEmpty())
    {
        const auto first = (*errors.getArray())[0];
        error = str(first.getProperty("message", {}));
        const auto code = str(first.getProperty("extensions", {}).getProperty("code", {}));
        if (isAuthError(res.status, code.empty() ? error : code)) unauthorized_ = true;
        if (error.empty()) error = "graphql error";
    }
    return parsed.getProperty("data", {});
}

SpliceSearchResult SpliceApi::parseSearch(const juce::String& json)
{
    SpliceSearchResult out;
    const auto data = juce::JSON::parse(json);
    const auto search = data.getProperty("data", data).getProperty("assetsSearch", {});
    if (! search.isObject()) { out.error = "no results"; return out; }
    if (auto* items = search.getProperty("items", {}).getArray())
        for (const auto& item : *items)
        {
            SpliceSample s;
            s.uuid = str(item.getProperty("uuid", {}));
            s.name = str(item.getProperty("name", {}));
            s.licensed = (bool) item.getProperty("licensed", false);
            s.bpm = (double) item.getProperty("bpm", 0);
            s.durationMs = (int) item.getProperty("duration", 0);
            s.key = str(item.getProperty("key", {}));
            s.chordType = str(item.getProperty("chord_type", {}));
            s.category = str(item.getProperty("asset_category_slug", {}));
            s.isLoop = s.category == "loop";
            if (auto* prices = item.getProperty("asset_prices", {}).getArray())
                for (const auto& p : *prices)
                    if (str(p.getProperty("currency", {})) == "Credits") s.priceCredits = (int) p.getProperty("amount", 0);
            if (auto* tags = item.getProperty("tags", {}).getArray())
                for (const auto& t : *tags) { auto l = str(t.getProperty("label", {})); if (! l.empty()) s.tags.push_back(l); }
            s.previewUrl = fileUrl(item.getProperty("files", {}), "preview_mp3");
            const auto pack = firstPack(item);
            if (pack.isObject())
            {
                s.packName = str(pack.getProperty("name", {}));
                s.packUuid = str(pack.getProperty("uuid", {}));
                s.providerName = str(pack.getProperty("provider", {}).getProperty("name", {}));
                s.coverUrl = fileUrl(pack.getProperty("files", {}), "cover_image");
            }
            if (! s.uuid.empty()) out.samples.push_back(std::move(s));
        }
    const auto pageMeta = search.getProperty("pagination_metadata", {});
    out.page = (int) pageMeta.getProperty("currentPage", 1);
    out.totalPages = (int) pageMeta.getProperty("totalPages", 0);
    out.totalRecords = (int) search.getProperty("response_metadata", {}).getProperty("records", 0);
    out.ok = true;
    return out;
}

SpliceCost SpliceApi::parseCost(const juce::String& json)
{
    SpliceCost out;
    const auto est = juce::JSON::parse(json).getProperty("data", {}).getProperty("costEstimate", {});
    if (! est.isObject()) return out;
    out.cost = (int) est.getProperty("cost", 0);
    out.userCredits = (int) est.getProperty("userCredits", 0);
    out.ok = true;
    return out;
}

std::string SpliceApi::parsePurchase(const juce::String& json, const std::string& uuid)
{
    const auto arr = juce::JSON::parse(json).getProperty("data", {}).getProperty("purchaseAssets", {});
    if (auto* items = arr.getArray())
        for (const auto& r : *items)
            if (uuid.empty() || str(r.getProperty("assetUuid", {})) == uuid)
            {
                const auto asset = r.getProperty("asset", {});
                if ((bool) r.getProperty("purchased", false) || (bool) asset.getProperty("licensed", false))
                    return str(asset.getProperty("uuid", {}));
            }
    return {};
}

SpliceApi::DownloadInfo SpliceApi::parsePremium(const juce::String& json)
{
    DownloadInfo out;
    const auto sample = juce::JSON::parse(json).getProperty("sample", {});
    if (! sample.isObject()) return out;
    out.url = str(sample.getProperty("url", {}));
    out.path = str(sample.getProperty("path", {}));
    out.size = (int64_t) sample.getProperty("file_size", 0);
    out.ok = ! out.url.empty();
    return out;
}

SpliceSearchResult SpliceApi::searchSamples(const std::string& token, const SpliceSearchQuery& q,
                                            const std::atomic<bool>& cancelled) const
{
    auto* vars = new juce::DynamicObject();
    vars->setProperty("query", q.text.empty() ? juce::var() : juce::var(juce::String::fromUTF8(q.text.c_str())));
    vars->setProperty("page", q.page);
    vars->setProperty("limit", q.limit);
    if (! q.key.empty()) vars->setProperty("key", juce::String(q.key));
    if (! q.chordType.empty()) vars->setProperty("chord_type", juce::String(q.chordType));
    if (! q.category.empty()) vars->setProperty("asset_category_slug", juce::String(q.category));
    if (q.minBpm > 0) vars->setProperty("min_bpm", q.minBpm);
    if (q.maxBpm > 0) vars->setProperty("max_bpm", q.maxBpm);
    if (q.semantic > 0) vars->setProperty("semantic", (double) q.semantic); // SPL-9: "describe the sound" search

    std::string error;
    const auto data = graphql(token, "SamplesSearch", kSearchQuery, juce::var(vars), cancelled, error);
    if (! error.empty()) { SpliceSearchResult r; r.error = error; return r; }
    // parseSearch accepts either {data:{assetsSearch}} or {assetsSearch}; wrap the data node.
    auto* wrap = new juce::DynamicObject();
    wrap->setProperty("assetsSearch", data.getProperty("assetsSearch", {}));
    return parseSearch(juce::JSON::toString(juce::var(wrap)));
}

SpliceCost SpliceApi::costEstimate(const std::string& token, const std::vector<std::string>& uuids,
                                   const std::atomic<bool>& cancelled) const
{
    juce::Array<juce::var> list;
    for (const auto& u : uuids) list.add(juce::String(u));
    auto* vars = new juce::DynamicObject();
    vars->setProperty("assetUuids", list);
    std::string error;
    const auto data = graphql(token, "AssetsCostEstimate", kCostQuery, juce::var(vars), cancelled, error);
    if (! error.empty()) return {};
    auto* wrap = new juce::DynamicObject();
    auto* outer = new juce::DynamicObject();
    outer->setProperty("costEstimate", data.getProperty("costEstimate", {}));
    wrap->setProperty("data", juce::var(outer));
    return parseCost(juce::JSON::toString(juce::var(wrap)));
}

int SpliceApi::creditBalance(const std::string& token, const std::atomic<bool>& cancelled) const
{
    std::string error;
    const auto data = graphql(token, "CreditsStoreQuery", kBalanceQuery, juce::var(new juce::DynamicObject()), cancelled, error);
    if (! error.empty()) return -1;
    const auto balance = data.getProperty("creditBalance", {});
    return balance.isVoid() ? -1 : (int) balance;
}

std::string SpliceApi::purchaseAsset(const std::string& token, const std::string& uuid,
                                     const std::atomic<bool>& cancelled) const
{
    juce::Array<juce::var> list; list.add(juce::String(uuid));
    auto* vars = new juce::DynamicObject();
    vars->setProperty("uuids", list);
    std::string error;
    const auto data = graphql(token, "AssetSharedPurchaseAssets", kPurchaseMutation, juce::var(vars), cancelled, error);
    if (! error.empty()) return {};
    auto* wrap = new juce::DynamicObject();
    auto* outer = new juce::DynamicObject();
    outer->setProperty("purchaseAssets", data.getProperty("purchaseAssets", {}));
    wrap->setProperty("data", juce::var(outer));
    return parsePurchase(juce::JSON::toString(juce::var(wrap)), uuid);
}

SpliceApi::DownloadInfo SpliceApi::premiumSample(const std::string& token, const std::string& fileHash,
                                                 const std::atomic<bool>& cancelled) const
{
    HttpRequest req;
    req.url = endpoints_.api + "/v2/premium/samples/" + juce::String(fileHash);
    req.method = "GET";
    req.headers.set("User-Agent", endpoints_.userAgent);
    req.headers.set("Authorization", "Bearer " + juce::String(token));
    const auto res = postNoRedirect(req, cancelled);
    if (res.outcome == HttpResponse::Outcome::Completed && isAuthError(res.status, {})) unauthorized_ = true;
    if (res.outcome != HttpResponse::Outcome::Completed || res.status < 200 || res.status >= 300) return {};
    return parsePremium(res.body);
}

bool SpliceApi::isAuthError(int status, const std::string& graphqlError)
{
    if (status == 401) return true; // 403 is Cloudflare's UA block (error 1010), not an expired session
    const auto e = juce::String(graphqlError).toLowerCase();
    return e.contains("unauthenticated") || e.contains("unauthorized") || e.contains("not authenticated")
        || e.contains("jwt expired") || e.contains("invalid token");
}

} // namespace sonora::platform::splice
