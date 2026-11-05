// LIB-1: sample packs (docs/SAMPLE_PACKS.md): manifest rules, the signed catalogue, resumable downloads against a loopback
// server, install/remove, and the PackStore worker end to end.
#include "TestFramework.h"
#include "core/Ed25519.h"
#include "core/Sha256.h"
#include "packs/PackStore.h"
#include "platform/HttpClient.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#if SONORA_HAVE_MONOCYPHER
#include "monocypher-ed25519.h"
#endif
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

using namespace sonora;

namespace
{
// Loopback HTTP/1.1 file server: GET with "Range: bytes=N-" (206), "/redirect/<path>" (302), and `cutAfter` to drop the
// connection after that many body bytes of the next response (a network failure mid-download).
class RangeServer final : juce::Thread
{
public:
    RangeServer() : juce::Thread("pack fixture")
    {
#if ! defined(_WIN32)
        std::signal(SIGPIPE, SIG_IGN); // a client that hangs up mid-body (size cap, cancel) must not end the test run
#endif
        listening_ = listener_.createListener(0, "127.0.0.1");
        startThread();
    }
    ~RangeServer() override
    {
        signalThreadShouldExit();
        listener_.close();
        stopThread(4000);
    }
    bool listening() const { return listening_; }
    juce::String url(const juce::String& path) const { return "http://127.0.0.1:" + juce::String(listener_.getBoundPort()) + path; }
    void put(const juce::String& path, std::string body) { const std::lock_guard lock(mutex_); files_[path] = std::move(body); }
    std::atomic<int64_t> cutAfter{-1};
    std::atomic<int> requests{0}, ranged{0};
    std::atomic<bool> ignoreRange{false};

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            std::unique_ptr<juce::StreamingSocket> client(listener_.waitForNextConnection());
            if (client == nullptr) continue;
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos && client->waitUntilReady(true, 2000) == 1)
            {
                const int n = client->read(buffer, (int) sizeof buffer, false);
                if (n <= 0) break;
                request.append(buffer, (size_t) n);
            }
            if (request.find("\r\n\r\n") == std::string::npos) continue;
            ++requests;
            const juce::String headers(request.substr(0, request.find("\r\n\r\n")));
            auto path = headers.fromFirstOccurrenceOf(" ", false, false).upToFirstOccurrenceOf(" ", false, false);
            if (path.startsWith("/redirect/"))
            {
                const auto response = "HTTP/1.1 302 Found\r\nLocation: " + url(path.substring(9)) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                client->write(response.toRawUTF8(), (int) response.getNumBytesAsUTF8());
                continue;
            }
            std::string body;
            bool found = false;
            {
                const std::lock_guard lock(mutex_);
                if (const auto file = files_.find(path); file != files_.end()) { body = file->second; found = true; }
            }
            if (! found)
            {
                const juce::String response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                client->write(response.toRawUTF8(), (int) response.getNumBytesAsUTF8());
                continue;
            }
            const auto rangeLine = headers.fromFirstOccurrenceOf("range: bytes=", false, true).upToFirstOccurrenceOf("-", false, false).trim();
            int64_t start = 0;
            juce::String status = "200 OK", extra;
            if (rangeLine.isNotEmpty() && ! ignoreRange)
            {
                ++ranged;
                start = rangeLine.getLargeIntValue();
                if (start >= (int64_t) body.size())
                {
                    const auto response = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + juce::String((int64_t) body.size())
                        + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    client->write(response.toRawUTF8(), (int) response.getNumBytesAsUTF8());
                    continue;
                }
                status = "206 Partial Content";
                extra = "Content-Range: bytes " + juce::String(start) + "-" + juce::String((int64_t) body.size() - 1) + "/"
                    + juce::String((int64_t) body.size()) + "\r\n";
            }
            const auto length = (int64_t) body.size() - start;
            const auto head = "HTTP/1.1 " + status + "\r\nContent-Type: application/octet-stream\r\nContent-Length: " + juce::String(length)
                + "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n" + extra + "\r\n";
            client->write(head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
            int64_t send = length;
            const int64_t cut = cutAfter.exchange(-1);
            if (cut >= 0) send = std::min(send, cut);
            int64_t sent = 0;
            int stalls = 0;
            while (sent < send && ! threadShouldExit() && stalls < 50)
            {
                // Accepted sockets may be non-blocking: a full send buffer returns -1 (EAGAIN) until the client reads.
                const int written = client->write(body.data() + start + sent, (int) std::min<int64_t>(16384, send - sent));
                if (written <= 0) { ++stalls; client->waitUntilReady(false, 100); continue; }
                stalls = 0;
                sent += written;
            }
            if (cut >= 0) juce::Thread::sleep(300); // let the client take the bytes before the early FIN
            finish(*client);
        }
    }
    // A clean FIN, then wait for the client to hang up: closing at once can reset the connection and lose the tail.
    static void finish(juce::StreamingSocket& client)
    {
#if defined(_WIN32)
        ::shutdown((SOCKET) client.getRawSocketHandle(), SD_SEND);
#else
        ::shutdown(client.getRawSocketHandle(), SHUT_WR);
#endif
        char scratch[1024];
        while (client.waitUntilReady(true, 2000) == 1 && client.read(scratch, (int) sizeof scratch, false) > 0) {}
        client.close();
    }
    juce::StreamingSocket listener_;
    bool listening_ = false;
    std::mutex mutex_;
    std::map<juce::String, std::string> files_;
};

std::string bytesOf(const juce::MemoryBlock& block) { return std::string(static_cast<const char*>(block.getData()), block.getSize()); }

std::string pseudoRandom(size_t size, uint32_t seed)
{
    std::string out(size, '\0');
    for (auto& c : out) { seed = seed * 1664525u + 1013904223u; c = (char) (seed >> 24); }
    return out;
}

struct TestKey
{
    std::string publicKey;
    uint8_t secret[64]{};
    TestKey()
    {
#if SONORA_HAVE_MONOCYPHER
        uint8_t seed[32], pub[32];
        for (int i = 0; i < 32; ++i) seed[i] = (uint8_t) (i * 7 + 3);
        crypto_ed25519_key_pair(secret, pub, seed);
        publicKey = juce::Base64::toBase64(pub, sizeof pub).toStdString();
#endif
    }
    std::string sign(const std::string& message) const
    {
#if SONORA_HAVE_MONOCYPHER
        uint8_t signature[64];
        crypto_ed25519_sign(signature, secret, (const uint8_t*) message.data(), message.size());
        return juce::Base64::toBase64(signature, sizeof signature).toStdString();
#else
        (void) message;
        return {};
#endif
    }
};

packs::PackContents fixtureContents(const std::string& id = "sonora-test-kit", const std::string& version = "1.0")
{
    packs::PackContents contents;
    contents.info = {id, "Sonora Test Kit", version, "A pack for the tests", "Sonora contributors", "CC0-1.0", {"House"}, {"Drums"}, {"909"}};
    contents.source = "made by tests/test_sample_packs.cpp";
    contents.files = {{"Kicks/Kick 01.wav", {{"Sound", "Kick"}}}, {"Hats/Hat 01.wav", {{"Sound", "Hi-hat"}}}};
    return contents;
}

// A zip with the given entries (name -> data); stored, not compressed, so byte sizes are predictable.
std::string zipOf(const std::vector<std::pair<juce::String, std::string>>& entries)
{
    juce::ZipFile::Builder builder;
    for (const auto& [name, data] : entries)
        builder.addEntry(std::make_unique<juce::MemoryInputStream>(data.data(), data.size(), true), 0, name, juce::Time::getCurrentTime());
    juce::MemoryOutputStream out;
    builder.writeToStream(out, nullptr);
    return bytesOf(out.getMemoryBlock());
}

std::string packArchive(const packs::PackContents& contents, bool licence = true, size_t loopBytes = 0)
{
    std::vector<std::pair<juce::String, std::string>> entries{{"pack.json", packs::packContentsToJson(contents)},
                                                              {"Kicks/Kick 01.wav", pseudoRandom(40000, 1)},
                                                              {"Hats/Hat 01.wav", pseudoRandom(30000, 2)}};
    if (loopBytes > 0) entries.push_back({"Loops/Loop 01.wav", pseudoRandom(loopBytes, 4)});
    if (licence) entries.push_back({"LICENSE.txt", "CC0 1.0 Universal"});
    return zipOf(entries);
}

packs::PackEntry entryFor(const packs::PackContents& contents, const std::string& archive, const std::string& url)
{
    packs::PackEntry entry;
    entry.info = contents.info;
    entry.url = url;
    entry.bytes = (int64_t) archive.size();
    entry.sha256 = crypto::toHex(crypto::sha256(archive));
    entry.installedBytes = 80000;
    entry.files = (int) contents.files.size();
    return entry;
}

struct TempFolder
{
    juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("sonora-packs-test-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    TempFolder() { folder.createDirectory(); }
    ~TempFolder() { folder.deleteRecursively(); }
};
} // namespace

TEST_CASE("packs: catalogue round trip keeps every field and skips packs that break a rule")
{
    const auto contents = fixtureContents();
    packs::Catalogue catalogue;
    catalogue.packs.push_back(entryFor(contents, "archive bytes", "sonora-test-kit-1.0.zip"));
    const auto json = packs::catalogueToJson(catalogue);
    std::string error;
    const auto parsed = packs::parseCatalogue(json, error);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->packs.size() == 1);
    const auto& entry = parsed->packs[0];
    CHECK(entry.info.id == "sonora-test-kit");
    CHECK(entry.info.name == "Sonora Test Kit");
    CHECK(entry.info.licence == "CC0-1.0");
    CHECK(entry.info.styles == std::vector<std::string>{"House"});
    CHECK(entry.info.types == std::vector<std::string>{"Drums"});
    CHECK(entry.info.tags == std::vector<std::string>{"909"});
    CHECK(entry.bytes == 13);
    CHECK(entry.sha256 == catalogue.packs[0].sha256);
    CHECK(entry.installedBytes == 80000);
    CHECK(entry.files == 2);
    CHECK(parsed->warnings.empty());

    // The licence rule, ids, hashes, sizes and duplicates are checked per pack; the rest of the list still loads.
    auto broken = juce::String(json).replace("\"packs\": [", "\"packs\": [{\"id\": \"cc-by-kit\", \"name\": \"X\", \"version\": \"1\", "
        "\"licence\": \"CC-BY-4.0\", \"archive\": {\"url\": \"x.zip\", \"bytes\": 1, \"sha256\": \"" + juce::String(entry.sha256) + "\"}}, "
        "{\"id\": \"Bad Id\", \"name\": \"X\", \"version\": \"1\", \"licence\": \"CC0-1.0\"}, "
        "{\"id\": \"no-hash\", \"name\": \"X\", \"version\": \"1\", \"licence\": \"CC0-1.0\", \"archive\": {\"url\": \"x.zip\", \"bytes\": 1}}, ");
    const auto withBroken = packs::parseCatalogue(broken.toStdString(), error);
    REQUIRE(withBroken.has_value());
    CHECK(withBroken->packs.size() == 1);
    CHECK(withBroken->warnings.size() == 3);
    CHECK(! packs::parseCatalogue("{\"format\": 2, \"packs\": []}", error).has_value());
    CHECK(error.find("newer format") != std::string::npos);
    CHECK(! packs::parseCatalogue("not json", error).has_value());

    const auto packJson = packs::packContentsToJson(contents);
    const auto back = packs::parsePackContents(packJson, error);
    REQUIRE(back.has_value());
    CHECK(back->files.size() == 2);
    CHECK(back->files[0].path == "Kicks/Kick 01.wav");
    CHECK(back->files[0].tags == (std::vector<std::pair<std::string, std::string>>{{"Sound", "Kick"}}));
    CHECK(back->source == contents.source);
}

TEST_CASE("packs: licence rule, ids and archive paths")
{
    CHECK(packs::licenceAllowed("CC0-1.0"));
    CHECK(! packs::licenceAllowed("CC-BY-4.0"));
    CHECK(! packs::licenceAllowed("CC-BY-SA-3.0"));
    CHECK(! packs::licenceAllowed(""));
    CHECK(packs::validPackId("sonora-house-kit"));
    CHECK(! packs::validPackId("-x"));
    CHECK(! packs::validPackId("House"));
    CHECK(! packs::validPackId("a/b"));
    CHECK(packs::safeRelativePath("Kicks/Kick 01.wav"));
    CHECK(packs::safeRelativePath("Kicks/"));
    CHECK(! packs::safeRelativePath("../evil.wav"));
    CHECK(! packs::safeRelativePath("Kicks/../../evil.wav"));
    CHECK(! packs::safeRelativePath("/etc/passwd"));
    CHECK(! packs::safeRelativePath("C:/x.wav"));
    CHECK(! packs::safeRelativePath("a\\b.wav"));
    CHECK(! packs::safeRelativePath("a//b.wav"));
    CHECK(! packs::safeRelativePath("./a.wav"));
    CHECK(packs::resolveArchiveUrl("https://sonoradaw.com/packs.json", "a.zip") == "https://sonoradaw.com/a.zip");
    CHECK(packs::resolveArchiveUrl("https://sonoradaw.com/packs.json", "https://github.com/x/a.zip") == "https://github.com/x/a.zip");
    CHECK(platform::downloadUrlAllowed("https://github.com/x.zip"));
    CHECK(platform::downloadUrlAllowed("http://127.0.0.1:8000/x.zip"));
    CHECK(platform::downloadUrlAllowed("http://localhost/x.zip"));
    CHECK(! platform::downloadUrlAllowed("http://example.org/x.zip"));
    CHECK(! platform::downloadUrlAllowed("ftp://example.org/x.zip"));
    int64_t total = 0;
    CHECK(platform::contentRangeStart("bytes 100-199/1000", total) == 100);
    CHECK(total == 1000);
    CHECK(platform::contentRangeStart("bytes 5-9/*", total) == 5);
    CHECK(total == 0);
    CHECK(platform::contentRangeStart("items 1-2/3", total) == -1);
}

TEST_CASE("packs: the catalogue signature covers every byte")
{
    if (! crypto::ed25519Available()) { std::printf("    (no Monocypher: skipped)\n"); return; }
    const TestKey key;
    const std::string json = packs::catalogueToJson({});
    const auto signature = key.sign(json);
    CHECK(packs::verifyCatalogue(json, signature, key.publicKey));
    auto tampered = json;
    tampered[tampered.size() / 2] ^= 1;
    CHECK(! packs::verifyCatalogue(tampered, signature, key.publicKey));
    CHECK(! packs::verifyCatalogue(json + " ", signature, key.publicKey));
    CHECK(! packs::verifyCatalogue(json, signature, TestKey().publicKey.substr(0, 10)));
    CHECK(! packs::verifyCatalogue(json, "", key.publicKey));
}

TEST_CASE("packs: downloads resume after a dropped connection, follow redirects and cancel")
{
    RangeServer server;
    REQUIRE(server.listening());
    const auto body = pseudoRandom(300000, 42);
    server.put("/a.zip", body);
    TempFolder temp;
    const auto file = temp.folder.getChildFile("a.zip.part");
    std::atomic<bool> cancelled{false};
    using Outcome = platform::HttpResponse::Outcome;
    platform::DownloadRequest request{server.url("/a.zip"), file, (int64_t) body.size(), 10, 5};

    server.cutAfter = 120000;
    int64_t lastProgress = -1;
    auto first = platform::downloadToFile(request, [&](int64_t got, int64_t) { lastProgress = got; }, cancelled);
    CHECK(first.outcome == Outcome::NetworkFailed);
    CHECK(file.getSize() == 120000);
    CHECK(first.fileBytes == 120000);

    auto second = platform::downloadToFile(request, [&](int64_t got, int64_t) { lastProgress = got; }, cancelled);
    CHECK(second.outcome == Outcome::Completed);
    CHECK(second.status == 206);
    CHECK(second.resumed);
    CHECK(server.ranged.load() == 1);
    CHECK(second.fileBytes == (int64_t) body.size());
    CHECK(lastProgress == (int64_t) body.size());
    juce::MemoryBlock data;
    file.loadFileAsData(data);
    CHECK(bytesOf(data) == body);

    // Complete already: the server says 416 and the file is left as it is.
    const auto third = platform::downloadToFile(request, {}, cancelled);
    CHECK(third.status == 416);
    CHECK(file.getSize() == (int64_t) body.size());

    // A server that ignores Range: the file is rewritten from the start, not appended to.
    file.deleteFile();
    server.cutAfter = 1000;
    platform::downloadToFile(request, {}, cancelled);
    server.ignoreRange = true;
    const auto full = platform::downloadToFile(request, {}, cancelled);
    CHECK(full.status == 200);
    CHECK(! full.resumed);
    CHECK(file.getSize() == (int64_t) body.size());
    server.ignoreRange = false;

    // Redirects are followed (GitHub release assets), 404 leaves the file alone, size caps hold.
    const auto redirected = temp.folder.getChildFile("b.part");
    CHECK(platform::downloadToFile({server.url("/redirect/a.zip"), redirected, (int64_t) body.size(), 10, 5}, {}, cancelled).status == 200);
    CHECK(redirected.getSize() == (int64_t) body.size());
    const auto missing = temp.folder.getChildFile("c.part");
    CHECK(platform::downloadToFile({server.url("/missing.zip"), missing, 1000, 10, 5}, {}, cancelled).status == 404);
    CHECK(! missing.existsAsFile());
    const auto capped = temp.folder.getChildFile("d.part");
    CHECK(platform::downloadToFile({server.url("/a.zip"), capped, 1000, 10, 5}, {}, cancelled).outcome == Outcome::TooLarge);
    CHECK(platform::downloadToFile({"http://example.org/a.zip", capped, 1000, 10, 5}, {}, cancelled).outcome == Outcome::InvalidUrl);
    cancelled = true;
    CHECK(platform::downloadToFile(request, {}, cancelled).outcome == Outcome::Cancelled);
}

TEST_CASE("packs: install unpacks a pack, replaces an older version, refuses unsafe archives and removes")
{
    TempFolder temp;
    const packs::PackLibrary library(temp.folder.getChildFile("Packs"));
    std::atomic<bool> cancelled{false};
    const auto contents = fixtureContents();
    const auto archiveData = packArchive(contents);
    const auto archive = temp.folder.getChildFile("kit.zip");
    archive.replaceWithData(archiveData.data(), archiveData.size());
    const auto entry = entryFor(contents, archiveData, "kit.zip");
    CHECK(packs::sha256OfFile(archive, cancelled) == entry.sha256);

    const auto installed = library.install(archive, entry, cancelled);
    CHECK(installed.ok);
    CHECK(installed.folder == library.root().getChildFile("Sonora Test Kit"));
    CHECK(installed.folder.getChildFile("Kicks/Kick 01.wav").getSize() == 40000);
    CHECK(installed.folder.getChildFile("LICENSE.txt").existsAsFile());
    CHECK(! library.privateFolder().getChildFile("staging").findChildFiles(juce::File::findFilesAndDirectories, false).size());
    REQUIRE(library.installed().size() == 1);
    CHECK(library.find("sonora-test-kit")->contents.files.size() == 2);

    // A new version replaces the old folder.
    const auto v2 = fixtureContents("sonora-test-kit", "1.1");
    const auto v2Data = packArchive(v2);
    archive.replaceWithData(v2Data.data(), v2Data.size());
    CHECK(library.install(archive, entryFor(v2, v2Data, "kit.zip"), cancelled).ok);
    REQUIRE(library.installed().size() == 1);
    CHECK(library.find("sonora-test-kit")->contents.info.version == "1.1");

    auto refused = [&](const std::string& data, const packs::PackEntry& expected) {
        archive.replaceWithData(data.data(), data.size());
        const auto result = library.install(archive, expected, cancelled);
        return ! result.ok && ! result.error.empty();
    };
    CHECK(refused(zipOf({{"pack.json", packs::packContentsToJson(contents)}, {"LICENSE.txt", "x"}, {"../evil.wav", "x"}}), entry));
    CHECK(refused(packArchive(contents, false), entry)); // no LICENSE.txt
    CHECK(refused(packArchive(fixtureContents("another-kit")), entry)); // pack.json is another pack
    auto ccBy = contents;
    ccBy.info.licence = "CC-BY-4.0";
    CHECK(refused(packArchive(ccBy), entry));
    auto small = entry;
    small.installedBytes = 1000; // unpacks to far more than the catalogue says
    CHECK(refused(zipOf({{"pack.json", packs::packContentsToJson(contents)}, {"LICENSE.txt", "x"}, {"Big.wav", pseudoRandom(3 << 20, 3)}}), small));
    CHECK(refused("not a zip", entry));
    CHECK(! temp.folder.getChildFile("evil.wav").exists());
    CHECK(library.find("sonora-test-kit")->contents.info.version == "1.1"); // a refused install leaves the old one

    std::string error;
    CHECK(library.remove("sonora-test-kit", error, false));
    CHECK(library.installed().empty());
    CHECK(! library.root().getChildFile("Sonora Test Kit").exists());
    CHECK(! library.remove("sonora-test-kit", error, false));
}

TEST_CASE("packs: the store fetches a signed catalogue, downloads with resume, verifies, installs and removes")
{
    if (! crypto::ed25519Available()) { std::printf("    (no Monocypher: skipped)\n"); return; }
    RangeServer server;
    REQUIRE(server.listening());
    const TestKey key;
    const auto contents = fixtureContents();
    const auto archive = packArchive(contents, true, 600000);
    packs::Catalogue catalogue;
    catalogue.packs.push_back(entryFor(contents, archive, "files/sonora-test-kit-1.0.zip"));
    catalogue.packs.back().installedBytes = 700000;
    auto broken = fixtureContents("sonora-broken-kit");
    broken.info.name = "Sonora Broken Kit";
    auto brokenEntry = entryFor(broken, packArchive(broken), "files/broken.zip");
    catalogue.packs.push_back(brokenEntry);
    const auto json = packs::catalogueToJson(catalogue);
    server.put("/packs.json", json);
    server.put("/packs.json.sig", key.sign(json));
    server.put("/files/sonora-test-kit-1.0.zip", archive);
    auto tampered = packArchive(broken);
    tampered[tampered.size() / 2] ^= 0x55; // same size, not the signed bytes
    server.put("/files/broken.zip", tampered);

    TempFolder temp;
    std::atomic<int> changes{0};
    packs::PackStore::Config config{server.url("/packs.json"), key.publicKey, temp.folder.getChildFile("Packs"), false, 10};
    using State = packs::PackStore::PackState;
    auto rowOf = [](const packs::PackStore& store, const std::string& id) {
        for (const auto& row : store.snapshot().rows)
            if (row.info.id == id) return std::optional(row);
        return std::optional<packs::PackStore::Row>();
    };
    {
        packs::PackStore store(config, [&] { ++changes; });
        CHECK(store.configured());
        store.waitUntilIdle();
        CHECK(store.snapshot().catalogue == packs::PackStore::CatalogueState::NotLoaded);
        CHECK(server.requests.load() == 0); // nothing is fetched until asked
        store.loadCatalogue();
        store.waitUntilIdle();
        CHECK(store.snapshot().catalogue == packs::PackStore::CatalogueState::Ready);
        REQUIRE(store.snapshot().rows.size() == 2);
        CHECK(rowOf(store, "sonora-test-kit")->state == State::Available);

        server.cutAfter = 300000; // the first try drops; the store retries and resumes
        store.download("sonora-test-kit");
        store.waitUntilIdle();
        const auto row = rowOf(store, "sonora-test-kit");
        REQUIRE(row.has_value());
        CHECK(row->state == State::Installed);
        CHECK(row->message.empty());
        CHECK(server.ranged.load() == 1);
        REQUIRE(row->installed.has_value());
        CHECK(row->installed->folder.getChildFile("Hats/Hat 01.wav").getSize() == 30000);
        CHECK(! store.library().partialDownload(catalogue.packs[0]).existsAsFile());
        CHECK(changes.load() > 0);

        store.download("sonora-broken-kit");
        store.waitUntilIdle();
        const auto failed = rowOf(store, "sonora-broken-kit");
        CHECK(failed->state == State::Failed);
        CHECK(failed->message.find("checksum") != std::string::npos);
        CHECK(! store.library().partialDownload(brokenEntry).existsAsFile());
        CHECK(! store.library().find("sonora-broken-kit").has_value());
    }
    {
        // A new store (an app restart) lists the cached, verified catalogue and the installed pack without the network.
        const int before = server.requests.load();
        packs::PackStore store(config, {});
        store.waitUntilIdle();
        CHECK(store.snapshot().catalogue == packs::PackStore::CatalogueState::Ready);
        CHECK(rowOf(store, "sonora-test-kit")->state == State::Installed);
        CHECK(server.requests.load() == before);
        store.remove("sonora-test-kit");
        store.waitUntilIdle();
        CHECK(rowOf(store, "sonora-test-kit")->state == State::Available);
        CHECK(! store.library().find("sonora-test-kit").has_value());
    }
    {
        // A catalogue signed by another key is refused and nothing from it is listed.
        server.put("/packs.json.sig", TestKey().sign(json + " "));
        juce::File(config.root).deleteRecursively();
        packs::PackStore store(config, {});
        store.loadCatalogue();
        store.waitUntilIdle();
        CHECK(store.snapshot().catalogue == packs::PackStore::CatalogueState::Failed);
        CHECK(store.snapshot().catalogueMessage.find("signature") != std::string::npos);
        CHECK(store.snapshot().rows.empty());
    }
    {
        // Unconfigured (no key): nothing can be loaded.
        packs::PackStore store({server.url("/packs.json"), "", temp.folder.getChildFile("Other"), false, 10}, {});
        CHECK(! store.configured());
        store.loadCatalogue();
        store.waitUntilIdle();
        CHECK(store.snapshot().catalogue == packs::PackStore::CatalogueState::Off);
    }
}
