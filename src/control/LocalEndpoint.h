#pragma once
#include "control/ControlService.h"
#include <juce_events/juce_events.h>
#include <atomic>

namespace sonora::control
{
// AGT-4: opt-in, same-user local automation. Requests execute only on the message thread.
class LocalEndpoint : private juce::Thread
{
public:
    LocalEndpoint();
    ~LocalEndpoint() override;
    ApplyResult enable(const std::string& instanceId, const std::string& documentId,
                       std::function<juce::var(const juce::var&)> handler);
    void disable();
    bool enabled() const { return isThreadRunning(); }
    juce::File registryFile() const { return registry_; }
    struct Activity { int active = 0; int64_t requests = 0, lastRequestMs = 0; };
    Activity activity() const;
private:
    void run() override;
    struct Shared;
    std::shared_ptr<Shared> shared_;
    juce::File socket_, registry_, credential_;
    std::string token_;
    int listener_ = -1;
};
}
