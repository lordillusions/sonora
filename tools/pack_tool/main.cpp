// pack_tool — builds, signs and tests Sonora's optional sample packs (LIB-1, docs/SAMPLE_PACKS.md).
//
//   pack_tool keygen <private-key-file>                  new Ed25519 key (base64 seed, owner-only file); prints the public key
//   pack_tool pubkey <private-key-file>                  prints the public key of an existing private key
//   pack_tool build <sources> <out> [--base-url URL] [--key <private-key-file>]
//         every folder in <sources> with a pack-source.json becomes <out>/<id>-<version>.zip; <out>/packs.json lists them
//         (archive URLs = base URL + file name, or just the file name: next to packs.json); --key also writes packs.json.sig
//   pack_tool sign <packs.json> --key <private-key-file>  (re)writes packs.json.sig
//   pack_tool verify <packs.json> --pubkey <base64>      checks packs.json.sig and lists what the catalogue holds
//   pack_tool client --feed <packs.json URL> --pubkey <base64> --root <folder> [--download <id>]... [--remove <id>]... [--cancel-after <bytes>]
//         the app's PackStore (the same code as the Browser's Packs list): load the catalogue, download/install/remove, print the rows
//
// Exit code 0 on success, 1 on any refused rule or failed step.

#include "core/Ed25519.h"
#include "core/Sha256.h"
#include "packs/PackStore.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>

#if SONORA_HAVE_MONOCYPHER
#include "monocypher-ed25519.h"
#include "monocypher.h"
#endif
#if ! defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace sonora;

namespace
{
const juce::StringArray kAudioExtensions{".wav", ".aif", ".aiff", ".flac", ".ogg", ".mp3"};
// Files a source folder may hold besides audio; pack-source.json is read, not shipped.
const juce::StringArray kShippedText{"LICENSE.txt", "README.txt", "SOURCES.txt"};

int fail(const juce::String& message)
{
    std::fprintf(stderr, "pack_tool: %s\n", message.toRawUTF8());
    return 1;
}

juce::String option(const juce::StringArray& args, const char* name)
{
    const int i = args.indexOf(name);
    return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
}

juce::StringArray options(const juce::StringArray& args, const char* name)
{
    juce::StringArray values;
    for (int i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name) values.add(args[i + 1]);
    return values;
}

std::string exact(const juce::File& file)
{
    juce::MemoryBlock data;
    file.loadFileAsData(data);
    return std::string(static_cast<const char*>(data.getData()), data.getSize());
}

#if SONORA_HAVE_MONOCYPHER
bool loadSecret(const juce::File& file, uint8_t secret[64], uint8_t pub[32])
{
    auto seed = crypto::fromBase64(file.loadFileAsString().trim().toStdString());
    if (seed.size() != 32) return false;
    crypto_ed25519_key_pair(secret, pub, seed.data()); // wipes the seed copy
    return true;
}

std::string sign(const std::string& message, const juce::File& keyFile)
{
    uint8_t secret[64], pub[32], signature[64];
    if (! loadSecret(keyFile, secret, pub)) return {};
    crypto_ed25519_sign(signature, secret, reinterpret_cast<const uint8_t*>(message.data()), message.size());
    crypto_wipe(secret, sizeof secret);
    return juce::Base64::toBase64(signature, sizeof signature).toStdString();
}
#endif

int keygen(const juce::File& file, bool create)
{
#if SONORA_HAVE_MONOCYPHER
    if (create)
    {
        if (file.exists()) return fail(file.getFullPathName() + " exists: refusing to overwrite a signing key");
        const auto seed = crypto::randomBytes(32);
        if (seed.size() != 32) return fail("no system random bytes");
        file.getParentDirectory().createDirectory();
        if (! file.replaceWithText(juce::Base64::toBase64(seed.data(), seed.size()) + "\n", false, false, "\n"))
            return fail("cannot write " + file.getFullPathName());
#if ! defined(_WIN32)
        ::chmod(file.getFullPathName().toRawUTF8(), 0600);
#endif
    }
    uint8_t secret[64], pub[32];
    if (! loadSecret(file, secret, pub)) return fail(file.getFullPathName() + " is not a pack_tool private key");
    crypto_wipe(secret, sizeof secret);
    std::printf("%s\n", juce::Base64::toBase64(pub, sizeof pub).toRawUTF8());
    return 0;
#else
    (void) file; (void) create;
    return fail("built without Monocypher (scripts/bootstrap.sh)");
#endif
}

bool tagsFromJson(const juce::var& object, std::vector<std::pair<std::string, std::string>>& tags)
{
    if (const auto* properties = object.getDynamicObject())
        for (const auto& property : properties->getProperties())
            tags.emplace_back(property.name.toString().toStdString(), property.value.toString().toStdString());
    return true;
}

// One source folder -> its archive. Every rule is checked before anything is written.
bool buildPack(const juce::File& folder, const juce::File& out, const juce::String& baseUrl, packs::PackEntry& entry)
{
    const auto sourceFile = folder.getChildFile("pack-source.json");
    juce::var source;
    if (juce::JSON::parse(sourceFile.loadFileAsString(), source).failed() || ! source.isObject())
    { fail(sourceFile.getFullPathName() + ": not a JSON object"); return false; }
    // pack-source.json is pack.json's fields plus folderTags/fileTags; reuse the pack.json rules for the shared fields.
    std::string error;
    juce::DynamicObject::Ptr probe = source.getDynamicObject()->clone().release();
    probe->setProperty("format", packs::kManifestFormat);
    probe->removeProperty("folderTags");
    probe->removeProperty("fileTags");
    auto info = packs::parsePackContents(juce::JSON::toString(probe.get()).toStdString(), error);
    if (! info) { fail(sourceFile.getFullPathName() + ": " + error); return false; }
    if (info->source.empty()) { fail(info->info.id + ": \"source\" must say where the files came from or how they were made"); return false; }
    const auto licence = folder.getChildFile("LICENSE.txt");
    if (! licence.existsAsFile() || licence.getSize() < 100) { fail(info->info.id + ": LICENSE.txt (the full licence text) is missing"); return false; }

    std::map<std::string, std::vector<std::pair<std::string, std::string>>> folderTags, fileTags;
    if (const auto* object = source.getProperty("folderTags", {}).getDynamicObject())
        for (const auto& property : object->getProperties()) tagsFromJson(property.value, folderTags[property.name.toString().toStdString()]);
    if (const auto* object = source.getProperty("fileTags", {}).getDynamicObject())
        for (const auto& property : object->getProperties()) tagsFromJson(property.value, fileTags[property.name.toString().toStdString()]);

    packs::PackContents contents{info->info, info->source, {}};
    juce::ZipFile::Builder zip;
    int64_t unpacked = 0;
    juce::Array<juce::File> files;
    for (const auto& entryFile : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles))
        files.add(entryFile.getFile());
    std::sort(files.begin(), files.end(), [&](const juce::File& a, const juce::File& b) {
        return a.getRelativePathFrom(folder).compareNatural(b.getRelativePathFrom(folder)) < 0;
    });
    for (const auto& file : files)
    {
        const auto relative = file.getRelativePathFrom(folder).replaceCharacter('\\', '/');
        if (relative == "pack-source.json" || file.getFileName() == ".DS_Store") continue;
        if (! packs::safeRelativePath(relative.toStdString()) || relative.startsWith(".") || relative.contains("/."))
        { fail(juce::String(info->info.id) + ": unsafe or hidden path " + relative); return false; }
        const bool audio = kAudioExtensions.contains(file.getFileExtension().toLowerCase());
        if (! audio && ! kShippedText.contains(relative)) { fail(juce::String(info->info.id) + ": " + relative + " is neither audio nor a licence/readme"); return false; }
        if (audio)
        {
            packs::PackFile packFile{relative.toStdString(), {}};
            // The nearest folder's tags win: "Hats/Open" before "Hats".
            auto parentOf = [](const juce::String& path) { return path.contains("/") ? path.upToLastOccurrenceOf("/", false, false) : juce::String(); };
            for (auto parent = parentOf(relative); parent.isNotEmpty(); parent = parentOf(parent))
                if (const auto tags = folderTags.find(parent.toStdString()); tags != folderTags.end())
                    for (const auto& tag : tags->second)
                        if (std::none_of(packFile.tags.begin(), packFile.tags.end(), [&](const auto& t) { return t.first == tag.first; }))
                            packFile.tags.push_back(tag);
            if (const auto tags = fileTags.find(relative.toStdString()); tags != fileTags.end())
                for (const auto& tag : tags->second)
                {
                    std::erase_if(packFile.tags, [&](const auto& t) { return t.first == tag.first; });
                    packFile.tags.push_back(tag);
                }
            contents.files.push_back(std::move(packFile));
        }
        unpacked += file.getSize();
        zip.addFile(file, audio ? 6 : 9, relative);
    }
    if (contents.files.empty()) { fail(info->info.id + ": no audio files"); return false; }
    const auto manifest = packs::packContentsToJson(contents);
    unpacked += (int64_t) manifest.size();
    zip.addEntry(std::make_unique<juce::MemoryInputStream>(manifest.data(), manifest.size(), true), 9, "pack.json", juce::Time::getCurrentTime());

    const auto name = juce::String(info->info.id + "-" + info->info.version + ".zip");
    const auto archive = out.getChildFile(name);
    archive.deleteFile();
    {
        juce::FileOutputStream stream(archive);
        if (! stream.openedOk() || ! zip.writeToStream(stream, nullptr)) { fail("cannot write " + archive.getFullPathName()); return false; }
    }
    const std::atomic<bool> never{false};
    entry.info = contents.info;
    entry.url = (baseUrl.isEmpty() ? name : baseUrl.trimCharactersAtEnd("/") + "/" + name).toStdString();
    entry.bytes = archive.getSize();
    entry.sha256 = packs::sha256OfFile(archive, never);
    entry.installedBytes = unpacked;
    entry.files = (int) contents.files.size();
    std::printf("%s %s  %d files  %.1f MB unpacked  %.1f MB archive  %s\n", entry.info.id.c_str(), entry.info.version.c_str(), entry.files,
                (double) unpacked / 1e6, (double) entry.bytes / 1e6, entry.sha256.c_str());
    return true;
}

int writeSignature(const juce::File& catalogue, const juce::File& keyFile)
{
#if SONORA_HAVE_MONOCYPHER
    const auto signature = sign(exact(catalogue), keyFile);
    if (signature.empty()) return fail(keyFile.getFullPathName() + " is not a pack_tool private key");
    const auto sigFile = catalogue.getSiblingFile(catalogue.getFileName() + ".sig");
    if (! sigFile.replaceWithText(juce::String(signature) + "\n", false, false, "\n")) return fail("cannot write " + sigFile.getFullPathName());
    std::printf("signed: %s\n", sigFile.getFullPathName().toRawUTF8());
    return 0;
#else
    (void) catalogue; (void) keyFile;
    return fail("built without Monocypher (scripts/bootstrap.sh)");
#endif
}

int build(const juce::File& sources, const juce::File& out, const juce::String& baseUrl, const juce::String& key)
{
    if (! sources.isDirectory()) return fail(sources.getFullPathName() + " is not a folder");
    if (! out.createDirectory()) return fail("cannot create " + out.getFullPathName());
    packs::Catalogue catalogue;
    auto folders = sources.findChildFiles(juce::File::findDirectories, false);
    folders.sort();
    for (const auto& folder : folders)
    {
        if (! folder.getChildFile("pack-source.json").existsAsFile()) continue;
        packs::PackEntry entry;
        if (! buildPack(folder, out, baseUrl, entry)) return 1;
        if (std::any_of(catalogue.packs.begin(), catalogue.packs.end(), [&](const auto& p) { return p.info.id == entry.info.id; }))
            return fail(entry.info.id + " is used by two source folders");
        catalogue.packs.push_back(std::move(entry));
    }
    if (catalogue.packs.empty()) return fail("no folder in " + sources.getFullPathName() + " has a pack-source.json");
    const auto file = out.getChildFile("packs.json");
    const auto json = packs::catalogueToJson(catalogue);
    if (! file.replaceWithData(json.data(), json.size())) return fail("cannot write " + file.getFullPathName());
    std::printf("catalogue: %s (%d packs)\n", file.getFullPathName().toRawUTF8(), (int) catalogue.packs.size());
    return key.isEmpty() ? 0 : writeSignature(file, juce::File::getCurrentWorkingDirectory().getChildFile(key));
}

int verify(const juce::File& catalogue, const juce::String& publicKey)
{
    const auto json = exact(catalogue);
    const auto signature = catalogue.getSiblingFile(catalogue.getFileName() + ".sig").loadFileAsString().trim().toStdString();
    if (! packs::verifyCatalogue(json, signature, publicKey.toStdString())) return fail("signature does not match");
    std::string error;
    const auto parsed = packs::parseCatalogue(json, error);
    if (! parsed) return fail(error);
    for (const auto& warning : parsed->warnings) std::printf("skipped: %s\n", warning.c_str());
    auto join = [](const std::vector<std::string>& values) {
        juce::StringArray list;
        for (const auto& value : values) list.add(juce::String::fromUTF8(value.c_str()));
        return list.joinIntoString(",");
    };
    for (const auto& entry : parsed->packs)
        std::printf("%s %s \"%s\" %s styles=%s types=%s %lld bytes %s\n", entry.info.id.c_str(), entry.info.version.c_str(),
                    entry.info.name.c_str(), entry.info.licence.c_str(), join(entry.info.styles).toRawUTF8(),
                    join(entry.info.types).toRawUTF8(), (long long) entry.bytes, entry.url.c_str());
    std::printf("signature ok, %d packs\n", (int) parsed->packs.size());
    return parsed->warnings.empty() ? 0 : 1;
}

void printRows(const packs::PackStore& store)
{
    const auto snapshot = store.snapshot();
    const char* catalogue[] = {"off", "not loaded", "loading", "ready", "failed"};
    std::printf("catalogue: %s%s%s\n", catalogue[(int) snapshot.catalogue], snapshot.catalogueMessage.empty() ? "" : ": ",
                snapshot.catalogueMessage.c_str());
    for (const auto& row : snapshot.rows)
        std::printf("  %-28s %-8s %-16s %lld/%lld%s%s%s\n", row.info.id.c_str(), row.info.version.c_str(), packs::toString(row.state),
                    (long long) row.received, (long long) row.total, row.installed ? "  " : "",
                    row.installed ? row.installed->folder.getFullPathName().toRawUTF8() : "", row.message.empty() ? "" : ("  " + row.message).c_str());
}

int client(const juce::StringArray& args)
{
    packs::PackStore::Config config{option(args, "--feed"), option(args, "--pubkey").toStdString(),
                                    juce::File::getCurrentWorkingDirectory().getChildFile(option(args, "--root")), false, 1000};
    if (config.catalogueUrl.isEmpty() || config.publicKey.empty() || option(args, "--root").isEmpty())
        return fail("client needs --feed, --pubkey and --root");
    const auto cancelAfter = option(args, "--cancel-after").getLargeIntValue();
    packs::PackStore* current = nullptr;
    std::string cancelling;
    packs::PackStore store(config, [&] {
        if (current == nullptr || cancelAfter <= 0 || cancelling.empty()) return;
        for (const auto& row : current->snapshot().rows)
            if (row.info.id == cancelling && row.state == packs::PackStore::PackState::Downloading && row.received >= cancelAfter)
            {
                const auto id = std::exchange(cancelling, std::string());
                std::printf("cancelling %s at %lld bytes\n", id.c_str(), (long long) row.received);
                current->cancel(id);
                return;
            }
    });
    current = &store;
    store.waitUntilIdle();
    store.loadCatalogue();
    store.waitUntilIdle();
    printRows(store);
    bool ok = store.snapshot().catalogue == packs::PackStore::CatalogueState::Ready;
    for (const auto& id : options(args, "--download"))
    {
        cancelling = cancelAfter > 0 ? id.toStdString() : std::string();
        const auto start = juce::Time::getMillisecondCounterHiRes();
        store.download(id.toStdString());
        store.waitUntilIdle();
        std::printf("download %s: %.2f s\n", id.toRawUTF8(), (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0);
        printRows(store);
        for (const auto& row : store.snapshot().rows)
            if (row.info.id == id.toStdString() && row.state != packs::PackStore::PackState::Installed && cancelAfter <= 0) ok = false;
    }
    for (const auto& id : options(args, "--remove"))
    {
        store.remove(id.toStdString());
        store.waitUntilIdle();
        std::printf("remove %s\n", id.toRawUTF8());
        printRows(store);
        if (store.library().find(id.toStdString())) ok = false;
    }
    current = nullptr;
    return ok ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(juce::String::fromUTF8(argv[i]));
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const auto command = args[0];
    if (command == "keygen" && args.size() >= 2) return keygen(cwd.getChildFile(args[1]), true);
    if (command == "pubkey" && args.size() >= 2) return keygen(cwd.getChildFile(args[1]), false);
    if (command == "build" && args.size() >= 3) return build(cwd.getChildFile(args[1]), cwd.getChildFile(args[2]), option(args, "--base-url"), option(args, "--key"));
    if (command == "sign" && args.size() >= 2 && option(args, "--key").isNotEmpty())
        return writeSignature(cwd.getChildFile(args[1]), cwd.getChildFile(option(args, "--key")));
    if (command == "verify" && args.size() >= 2) return verify(cwd.getChildFile(args[1]), option(args, "--pubkey"));
    if (command == "client") return client(args);
    std::fprintf(stderr, "usage: pack_tool keygen|pubkey|build|sign|verify|client ... (see tools/pack_tool/main.cpp)\n");
    return 2;
}
