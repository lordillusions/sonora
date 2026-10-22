// File-backed SecretStore (Linux, Windows). See SecretStore.h for where the file lives and its limits.
// The file is {"<service>": {"<account>": "<value>"}}, rewritten through an owner-only temporary file.

#include "platform/SecretStore.h"
#include "platform/Paths.h"

#include <cerrno>
#if ! defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sonora::platform
{
namespace
{
constexpr int64_t maxFileBytes = 1024 * 1024;

juce::File storeFile(const juce::File& folder)
{
    return (folder == juce::File() ? appDataFolder() : folder).getChildFile("Secrets.json");
}

bool load(const juce::File& file, juce::var& root)
{
    root = juce::var(new juce::DynamicObject());
    if (! file.existsAsFile()) return true;
    if (file.getSize() > maxFileBytes) return false;
    const auto parsed = juce::JSON::parse(file.loadFileAsString());
    if (! parsed.isObject()) return false;
    root = parsed;
    return true;
}

bool privateFolder(const juce::File& folder)
{
    if (! folder.createDirectory()) return false;
#if ! defined(_WIN32)
    ::chmod(folder.getFullPathName().toRawUTF8(), 0700);
#endif
    return true;
}

// Writes `text` to a fresh owner-only file next to `file`, then renames it over `file`.
bool save(const juce::File& file, const juce::var& root)
{
    if (! privateFolder(file.getParentDirectory())) return false;
    const auto text = juce::JSON::toString(root);
    const auto temp = file.getSiblingFile(file.getFileName() + "." + juce::Uuid().toString().substring(0, 8) + ".tmp");
#if defined(_WIN32)
    if (! temp.replaceWithText(text, false, false, "\n")) return false;
#else
    const int fd = ::open(temp.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const auto* bytes = text.toRawUTF8();
    const auto length = text.getNumBytesAsUTF8();
    const bool written = ::write(fd, bytes, length) == static_cast<ssize_t>(length);
    ::close(fd);
    if (! written) { temp.deleteFile(); return false; }
#endif
    if (temp.moveFileTo(file)) return true;
    temp.deleteFile();
    return false;
}

SecretStatus failed() { return {SecretStatus::Code::Failed, errno}; }
} // namespace

SecretStore::SecretStore(juce::File folder) : folder_(std::move(folder)) {}
const char* SecretStore::backendName() { return "Secret file"; }

bool SecretStore::contains(const SecretId& id) const
{
    juce::String value;
    return read(id, value).ok();
}

SecretStatus SecretStore::read(const SecretId& id, juce::String& value) const
{
    value.clear();
    if (id.service.isEmpty() || id.account.isEmpty()) return {SecretStatus::Code::NotFound, 0};
    juce::var root;
    if (! load(storeFile(folder_), root)) return failed();
    const auto entry = root[juce::Identifier(id.service)][juce::Identifier(id.account)];
    if (! entry.isString()) return {SecretStatus::Code::NotFound, 0};
    value = entry.toString();
    return {SecretStatus::Code::Ok, 0};
}

SecretStatus SecretStore::write(const SecretId& id, const juce::String& value)
{
    if (id.service.isEmpty() || id.account.isEmpty()) return {SecretStatus::Code::Failed, EINVAL};
    const auto file = storeFile(folder_);
    juce::var root;
    if (! load(file, root)) return failed();
    auto* object = root.getDynamicObject();
    if (! object->getProperty(id.service).isObject()) object->setProperty(id.service, juce::var(new juce::DynamicObject()));
    object->getProperty(id.service).getDynamicObject()->setProperty(id.account, value);
    return save(file, root) ? SecretStatus{SecretStatus::Code::Ok, 0} : failed();
}

SecretStatus SecretStore::remove(const SecretId& id)
{
    if (id.service.isEmpty() || id.account.isEmpty()) return {SecretStatus::Code::Ok, 0};
    const auto file = storeFile(folder_);
    juce::var root;
    if (! load(file, root)) return failed();
    auto* service = root[juce::Identifier(id.service)].getDynamicObject();
    if (service == nullptr || ! service->hasProperty(id.account)) return {SecretStatus::Code::Ok, 0};
    service->removeProperty(id.account);
    if (service->getProperties().isEmpty()) root.getDynamicObject()->removeProperty(id.service);
    return save(file, root) ? SecretStatus{SecretStatus::Code::Ok, 0} : failed();
}
} // namespace sonora::platform
