// LIB-1: packs.json / pack.json reading and writing (PackManifest.h).
#include "packs/PackManifest.h"

#include "core/Ed25519.h"

#include <algorithm>

namespace sonora::packs
{
namespace
{
std::string text(const juce::var& object, const char* name)
{
    const auto value = object.getProperty(name, {});
    return value.isString() ? value.toString().trim().toStdString() : std::string();
}

int64_t number(const juce::var& object, const char* name)
{
    const auto value = object.getProperty(name, {});
    return value.isInt() || value.isInt64() || value.isDouble() ? static_cast<int64_t>(static_cast<juce::int64>(value)) : -1; // long vs long long on Linux
}

bool shortLabel(const std::string& s, size_t limit) { return ! s.empty() && s.size() <= limit && s.find_first_of("\r\n\t") == std::string::npos; }

bool readLabels(const juce::var& object, const char* name, std::vector<std::string>& out, std::string& error)
{
    const auto value = object.getProperty(name, {});
    if (value.isVoid()) return true;
    const auto* array = value.getArray();
    if (array == nullptr) { error = std::string(name) + " is not a list"; return false; }
    for (const auto& item : *array)
    {
        const auto label = item.toString().trim().toStdString();
        if (! item.isString() || ! shortLabel(label, 40)) { error = std::string(name) + " holds an empty or long label"; return false; }
        if (std::find(out.begin(), out.end(), label) == out.end()) out.push_back(label);
    }
    return true;
}

bool readInfo(const juce::var& object, PackInfo& info, std::string& error)
{
    info.id = text(object, "id");
    info.name = text(object, "name");
    info.version = text(object, "version");
    info.description = text(object, "description");
    info.author = text(object, "author");
    info.licence = text(object, "licence");
    if (! validPackId(info.id)) { error = "bad id \"" + info.id + "\""; return false; }
    if (! shortLabel(info.name, 80)) { error = info.id + ": missing or long name"; return false; }
    if (info.version.empty() || info.version.size() > 20
        || ! juce::String(info.version).containsOnly("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-"))
    { error = info.id + ": bad version"; return false; }
    if (! licenceAllowed(info.licence)) { error = info.id + ": licence \"" + info.licence + "\" is not allowed"; return false; }
    if (info.description.size() > 2000) { error = info.id + ": description too long"; return false; }
    std::string listError;
    if (! readLabels(object, "styles", info.styles, listError) || ! readLabels(object, "types", info.types, listError)
        || ! readLabels(object, "tags", info.tags, listError))
    { error = info.id + ": " + listError; return false; }
    return true;
}

juce::var labels(const std::vector<std::string>& values)
{
    juce::Array<juce::var> array;
    for (const auto& value : values) array.add(juce::String::fromUTF8(value.c_str()));
    return array;
}

juce::DynamicObject::Ptr infoObject(const PackInfo& info)
{
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    object->setProperty("id", juce::String::fromUTF8(info.id.c_str()));
    object->setProperty("name", juce::String::fromUTF8(info.name.c_str()));
    object->setProperty("version", juce::String::fromUTF8(info.version.c_str()));
    if (! info.description.empty()) object->setProperty("description", juce::String::fromUTF8(info.description.c_str()));
    if (! info.author.empty()) object->setProperty("author", juce::String::fromUTF8(info.author.c_str()));
    object->setProperty("licence", juce::String::fromUTF8(info.licence.c_str()));
    object->setProperty("styles", labels(info.styles));
    object->setProperty("types", labels(info.types));
    if (! info.tags.empty()) object->setProperty("tags", labels(info.tags));
    return object;
}

std::optional<juce::var> parseRoot(const std::string& json, std::string& error)
{
    juce::var root;
    const auto result = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())), root);
    if (result.failed() || ! root.isObject()) { error = "not a JSON object"; return std::nullopt; }
    const auto format = number(root, "format");
    if (format < 1) { error = "no format number"; return std::nullopt; }
    if (format > kManifestFormat) { error = "a newer format (" + std::to_string(format) + "): update Sonora"; return std::nullopt; }
    return root;
}
} // namespace

const std::vector<std::string>& allowedLicences()
{
    // CC0 only: no attribution chain to keep and no share-alike terms on what users make with the samples.
    static const std::vector<std::string> licences{"CC0-1.0"};
    return licences;
}

bool licenceAllowed(std::string_view spdx)
{
    const auto& licences = allowedLicences();
    return std::find(licences.begin(), licences.end(), spdx) != licences.end();
}

bool validPackId(std::string_view id)
{
    if (id.empty() || id.size() > 64 || id.front() == '-' || id.back() == '-') return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool validSha256(std::string_view hex)
{
    return hex.size() == 64 && std::all_of(hex.begin(), hex.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool safeRelativePath(std::string_view path)
{
    if (path.empty() || path.size() > 400 || path.front() == '/' || path.find('\\') != std::string_view::npos
        || path.find(':') != std::string_view::npos || path.find('\0') != std::string_view::npos)
        return false;
    size_t start = 0;
    while (start <= path.size())
    {
        const auto end = std::min(path.find('/', start), path.size());
        const auto part = path.substr(start, end - start);
        if (part.empty()) return end == path.size() && end > 0; // only a trailing '/' (a folder entry) may be empty
        if (part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

std::optional<Catalogue> parseCatalogue(const std::string& json, std::string& error)
{
    const auto root = parseRoot(json, error);
    if (! root) return std::nullopt;
    Catalogue catalogue;
    const auto* packs = root->getProperty("packs", {}).getArray();
    if (packs == nullptr) { error = "no packs list"; return std::nullopt; }
    for (const auto& object : *packs)
    {
        PackEntry entry;
        std::string problem;
        if (! readInfo(object, entry.info, problem)) { catalogue.warnings.push_back(problem); continue; }
        const auto archive = object.getProperty("archive", {});
        entry.url = text(archive, "url");
        entry.sha256 = text(archive, "sha256");
        entry.bytes = number(archive, "bytes");
        entry.installedBytes = std::max<int64_t>(0, number(object, "installedBytes"));
        entry.files = static_cast<int>(std::max<int64_t>(0, number(object, "files")));
        if (entry.url.empty() || entry.url.find_first_of(" \r\n\t\"") != std::string::npos) problem = "no archive url";
        else if (! validSha256(entry.sha256)) problem = "no sha256";
        else if (entry.bytes <= 0 || entry.bytes > (int64_t(4) << 30)) problem = "bad archive size";
        else if (std::any_of(catalogue.packs.begin(), catalogue.packs.end(), [&](const auto& p) { return p.info.id == entry.info.id; }))
            problem = "listed twice";
        if (! problem.empty()) { catalogue.warnings.push_back(entry.info.id + ": " + problem); continue; }
        catalogue.packs.push_back(std::move(entry));
    }
    return catalogue;
}

std::string catalogueToJson(const Catalogue& catalogue)
{
    juce::Array<juce::var> packs;
    for (const auto& entry : catalogue.packs)
    {
        auto object = infoObject(entry.info);
        juce::DynamicObject::Ptr archive = new juce::DynamicObject();
        archive->setProperty("url", juce::String::fromUTF8(entry.url.c_str()));
        archive->setProperty("bytes", (juce::int64) entry.bytes);
        archive->setProperty("sha256", juce::String(entry.sha256));
        object->setProperty("archive", archive.get());
        object->setProperty("installedBytes", (juce::int64) entry.installedBytes);
        object->setProperty("files", entry.files);
        packs.add(object.get());
    }
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("format", kManifestFormat);
    root->setProperty("packs", packs);
    return juce::JSON::toString(root.get(), false).toStdString() + "\n";
}

std::optional<PackContents> parsePackContents(const std::string& json, std::string& error)
{
    const auto root = parseRoot(json, error);
    if (! root) return std::nullopt;
    PackContents contents;
    if (! readInfo(*root, contents.info, error)) return std::nullopt;
    contents.source = text(*root, "source");
    if (const auto* files = root->getProperty("files", {}).getArray())
        for (const auto& object : *files)
        {
            PackFile file;
            file.path = text(object, "path");
            if (! safeRelativePath(file.path) || file.path.back() == '/') { error = "bad file path \"" + file.path + "\""; return std::nullopt; }
            if (const auto* tags = object.getProperty("tags", {}).getDynamicObject())
                for (const auto& property : tags->getProperties())
                {
                    const auto group = property.name.toString().trim().toStdString(), value = property.value.toString().trim().toStdString();
                    if (shortLabel(group, 40) && shortLabel(value, 40)) file.tags.emplace_back(group, value);
                }
            contents.files.push_back(std::move(file));
        }
    return contents;
}

std::string packContentsToJson(const PackContents& contents)
{
    auto root = infoObject(contents.info);
    root->setProperty("format", kManifestFormat);
    if (! contents.source.empty()) root->setProperty("source", juce::String::fromUTF8(contents.source.c_str()));
    juce::Array<juce::var> files;
    for (const auto& file : contents.files)
    {
        juce::DynamicObject::Ptr object = new juce::DynamicObject();
        object->setProperty("path", juce::String::fromUTF8(file.path.c_str()));
        juce::DynamicObject::Ptr tags = new juce::DynamicObject();
        for (const auto& [group, value] : file.tags)
            tags->setProperty(juce::Identifier(juce::String::fromUTF8(group.c_str())), juce::String::fromUTF8(value.c_str()));
        object->setProperty("tags", tags.get());
        files.add(object.get());
    }
    root->setProperty("files", files);
    return juce::JSON::toString(root.get(), false).toStdString() + "\n";
}

juce::String resolveArchiveUrl(const juce::String& catalogueUrl, const std::string& url)
{
    const auto target = juce::String::fromUTF8(url.c_str());
    if (target.contains("://")) return target;
    const auto base = catalogueUrl.upToLastOccurrenceOf("/", true, false);
    return base.isEmpty() || target.startsWith("/") ? juce::String() : base + target;
}

bool verifyCatalogue(const std::string& json, std::string_view signatureBase64, std::string_view publicKeyBase64)
{
    return crypto::verifyEd25519(publicKeyBase64, json.data(), json.size(), signatureBase64);
}

} // namespace sonora::packs
