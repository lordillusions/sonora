#pragma once
#include "control/ControlService.h"

namespace sonora::agent
{
enum class Protocol { Responses, ChatCompletions, Anthropic };
struct Config
{
    juce::String provider = "openai", baseUrl = "https://api.openai.com/v1", model = "gpt-6.1-sol";
    Protocol protocol = Protocol::Responses;
    bool enabled = false;
};
struct Provider { const char* id; const char* name; const char* baseUrl; const char* model; Protocol protocol; };
const std::vector<Provider>& providers();
Config preset(const juce::String& id);
juce::String protocolName(Protocol);
control::ApplyResult validate(const Config&);
juce::String requestUrl(const Config&);
juce::var configJson(const Config&); // contains no credential
Config configFromJson(const juce::var&);
// Credentials are scoped to provider/protocol/base URL. Editing an endpoint cannot reuse another host's key.
class Settings
{
public:
    explicit Settings(juce::File file = {});
    Config load() const;
    bool hasKey(const Config&) const;
    control::ApplyResult readKey(const Config&, juce::String& key) const;
    control::ApplyResult save(const Config&, const juce::String& newKey);
    control::ApplyResult removeKey(const Config&);
private:
    juce::File file_;
};
}
