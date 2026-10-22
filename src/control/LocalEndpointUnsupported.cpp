// COM-7: Windows has no local control endpoint yet (macOS/Linux use a same-user Unix socket,
// LocalEndpoint.cpp). TODO(COM-7): named pipe with an owner-only DACL.
#include "control/LocalEndpoint.h"

namespace sonora::control
{
struct LocalEndpoint::Shared
{
};
LocalEndpoint::LocalEndpoint() : Thread("Sonora local control") {}
LocalEndpoint::~LocalEndpoint() { disable(); }
LocalEndpoint::Activity LocalEndpoint::activity() const { return {}; }
void LocalEndpoint::disable() {}
ApplyResult LocalEndpoint::enable(const std::string&, const std::string&, std::function<juce::var(const juce::var&)>)
{
    return ApplyResult::failure("ipc_unsupported", "Local MCP access is not available on this platform yet");
}
void LocalEndpoint::run() {}
} // namespace sonora::control
