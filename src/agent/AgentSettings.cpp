#include "agent/AgentConfig.h"
#include "platform/Paths.h"
#include "platform/SecretStore.h"
#if ! defined(_WIN32)
#include <sys/stat.h>
#endif

namespace sonora::agent
{
namespace
{
platform::SecretId secretId(const Config& config)
{
    return {"org.sonora.daw.native-agent", config.provider + ":" + protocolName(config.protocol) + ":" + config.baseUrl.trimCharactersAtEnd("/")};
}
control::ApplyResult keyError(const platform::SecretStatus& status)
{
    return control::ApplyResult::failure("keychain_error", juce::String(platform::SecretStore::backendName()).toStdString()
        + " could not access this provider credential (status " + std::to_string(status.osStatus) + ").");
}
}
Settings::Settings(juce::File file) : file_(file == juce::File() ? platform::appDataFolder().getChildFile("Agent.json") : file) {}
Config Settings::load() const
{
    if (! file_.existsAsFile() || file_.getSize() > 16384) return {};
    return configFromJson(juce::JSON::parse(file_));
}
bool Settings::hasKey(const Config& config) const
{
    return platform::SecretStore().contains(secretId(config));
}
control::ApplyResult Settings::readKey(const Config& config, juce::String& key) const
{
    key.clear();
    const auto status = platform::SecretStore().read(secretId(config), key);
    if (! status.ok()) return keyError(status);
    if (key.isEmpty()) return control::ApplyResult::failure("missing_key", "Save an API key for this provider and endpoint.");
    return control::ApplyResult::success();
}
control::ApplyResult Settings::save(const Config& config, const juce::String& newKey)
{
    if (const auto checked = validate(config); ! checked.ok) return checked;
    if (newKey.isNotEmpty())
    {
        if (newKey.length() > 8192 || ! newKey.containsOnly("!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"))
            return control::ApplyResult::failure("invalid_key", "API keys must be bounded printable tokens without whitespace.");
        if (const auto status = platform::SecretStore().write(secretId(config), newKey); ! status.ok()) return keyError(status);
    }
    if (config.enabled && ! hasKey(config)) return control::ApplyResult::failure("missing_key", "Save an API key before enabling the native LLM.");
    if (! file_.getParentDirectory().createDirectory() || ! file_.replaceWithText(juce::JSON::toString(configJson(config))))
        return control::ApplyResult::failure("settings_write_failed", "Could not save native LLM settings.");
#if ! defined(_WIN32)
    ::chmod(file_.getFullPathName().toRawUTF8(), 0600);
#endif
    return control::ApplyResult::success();
}
control::ApplyResult Settings::removeKey(const Config& config)
{
    const auto status = platform::SecretStore().remove(secretId(config));
    return status.ok() ? control::ApplyResult::success() : keyError(status);
}
}
