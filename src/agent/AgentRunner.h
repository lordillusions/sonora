#pragma once
#include "agent/AgentProtocol.h"
#include <atomic>
#include <thread>

namespace sonora::agent
{
struct HttpResult { juce::var response; juce::String error; };
using Transport = std::function<HttpResult(const Config&, const juce::String& key, const juce::var&, const std::atomic<bool>& cancelled)>;
HttpResult post(const Config&, const juce::String& key, const juce::var&, const std::atomic<bool>& cancelled);
class Runner
{
public:
    struct Events
    {
        std::function<void(const juce::String&)> text;
        std::function<void(const juce::String&, const juce::var&)> tool;
        std::function<void(const juce::String&)> finished;
    };
    explicit Runner(Transport transport = post);
    ~Runner();
    control::ApplyResult start(control::ControlService&, Config, juce::String key, juce::String prompt, Events);
    void cancel();
    bool running() const;
private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread worker_;
    Transport transport_;
};
}
