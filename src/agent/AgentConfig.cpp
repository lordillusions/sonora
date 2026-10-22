#include "agent/AgentConfig.h"

namespace sonora::agent
{
const std::vector<Provider>& providers()
{
    static const std::vector<Provider> presets = {
        {"openai", "OpenAI", "https://api.openai.com/v1", "gpt-6.1-sol", Protocol::Responses},
        {"anthropic", "Anthropic", "https://api.anthropic.com/v1", "claude-sonnet-5-5", Protocol::Anthropic},
        {"gemini", "Gemini", "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-3.8-flash", Protocol::ChatCompletions},
        {"deepseek", "DeepSeek", "https://api.deepseek.com", "deepseek-flash", Protocol::ChatCompletions},
        {"glm", "GLM / Z.ai", "https://api.z.ai/api/paas/v4", "glm-5.3", Protocol::ChatCompletions},
        {"openrouter", "OpenRouter", "https://openrouter.ai/api/v1", "~openai/gpt-sol-latest", Protocol::ChatCompletions},
        {"custom", "Custom / OpenAI compatible", "", "", Protocol::ChatCompletions}
    };
    return presets;
}
Config preset(const juce::String& id)
{
    for (const auto& p : providers()) if (id == p.id) return {p.id, p.baseUrl, p.model, p.protocol, false};
    return {};
}
juce::String protocolName(Protocol protocol)
{
    switch (protocol)
    {
        case Protocol::Responses: return "responses";
        case Protocol::ChatCompletions: return "chat-completions";
        case Protocol::Anthropic: return "anthropic-messages";
    }
    return {};
}
control::ApplyResult validate(const Config& config)
{
    const juce::URL url(config.baseUrl);
    const auto scheme = url.getScheme(), host = url.getDomain();
    const bool local = host == "localhost" || host == "127.0.0.1" || host == "[::1]" || host == "::1";
    if ((scheme != "https" && ! (scheme == "http" && local)) || host.isEmpty()
        || config.baseUrl.containsAnyOf("\r\n\t ?#@\\") || config.baseUrl.length() > 2048)
        return control::ApplyResult::failure("invalid_endpoint", "Use an HTTPS base URL without credentials, query or fragment (HTTP is allowed only on loopback).");
    if (config.model.trim().isEmpty() || config.model.length() > 256 || config.model.containsAnyOf("\r\n\t"))
        return control::ApplyResult::failure("invalid_model", "Enter a model ID supported by this endpoint.");
    if (std::none_of(providers().begin(), providers().end(), [&](const auto& p) { return config.provider == p.id; }))
        return control::ApplyResult::failure("invalid_provider", "Choose a provider preset or Custom.");
    return control::ApplyResult::success();
}
juce::String requestUrl(const Config& config)
{
    const auto base = config.baseUrl.trimCharactersAtEnd("/");
    switch (config.protocol)
    {
        case Protocol::Responses: return base + "/responses";
        case Protocol::ChatCompletions: return base + "/chat/completions";
        case Protocol::Anthropic: return base + "/messages";
    }
    return {};
}
juce::var configJson(const Config& c)
{
    return control::object({{"provider", c.provider}, {"baseUrl", c.baseUrl.trimCharactersAtEnd("/")}, {"model", c.model},
        {"protocol", protocolName(c.protocol)}, {"enabled", c.enabled}});
}
Config configFromJson(const juce::var& json)
{
    auto result = preset(json["provider"].toString());
    result.provider = json["provider"].toString(); result.baseUrl = json["baseUrl"].toString(); result.model = json["model"].toString();
    const auto protocol = json["protocol"].toString();
    if (protocol == "responses") result.protocol = Protocol::Responses;
    else if (protocol == "chat-completions") result.protocol = Protocol::ChatCompletions;
    else if (protocol == "anthropic-messages") result.protocol = Protocol::Anthropic;
    else { result.enabled = false; return result; }
    result.enabled = json["enabled"].isBool() && static_cast<bool>(json["enabled"]) && validate(result).ok;
    return result;
}
}
