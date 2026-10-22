#pragma once
#include "agent/AgentConfig.h"
#include <set>

namespace sonora::agent
{
struct ToolCall { juce::String id, name; juce::var arguments; };
struct Turn { juce::String text; std::vector<ToolCall> calls; };
// AGT-6a: provider framing only. Credentials/network/application mutation never enter this class.
class Conversation
{
public:
    Conversation(Config, juce::String prompt);
    juce::var request() const;
    Turn receive(const juce::var& response); // throws on incomplete/malformed/unsupported responses
    void addResult(const ToolCall&, const juce::var&);
private:
    Config config_;
    juce::Array<juce::var> messages_;
    std::set<std::string> seenCalls_;
};
juce::var tools();
juce::String operationForTool(const juce::String&);
// Binds tools to one launch/document. Does not upgrade stale revisions supplied by the model.
juce::var executeTool(control::ControlService&, const ToolCall&, const juce::String& document, const juce::String& operationKey);
juce::String describeResult(const juce::String& source, const juce::String& operation, const juce::var&);
juce::var parseJson(const juce::String&, int maxBytes = 4 * 1024 * 1024);
}
