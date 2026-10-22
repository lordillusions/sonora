#include "agent/AgentRunner.h"
#include <juce_events/juce_events.h>
#include <condition_variable>
#include <mutex>
#include <chrono>

namespace sonora::agent
{
struct Runner::State
{
    std::atomic<bool> alive{true}, cancelled{false}, running{true};
    Events events;
    juce::String key, document, runId = juce::Uuid().toString();
    control::ControlService* service = nullptr; // only dereferenced inside guarded message-thread callbacks
    std::function<void()> interruptWait;
    std::mutex mutex;
};
Runner::Runner(Transport transport) : transport_(std::move(transport)) {}
Runner::~Runner()
{
    if (state_) state_->alive = false;
    cancel();
    if (worker_.joinable()) worker_.join();
}
bool Runner::running() const { return state_ && state_->running; }
void Runner::cancel()
{
    if (! state_) return;
    state_->cancelled = true;
    const std::lock_guard lock(state_->mutex);
    if (state_->interruptWait) state_->interruptWait();
}
control::ApplyResult Runner::start(control::ControlService& service, Config config, juce::String key, juce::String prompt, Events events)
{
    if (running()) return control::ApplyResult::failure("busy", "A native LLM request is already running");
    if (const auto checked = validate(config); ! checked.ok) return checked;
    if (! config.enabled || key.isEmpty() || prompt.trim().isEmpty() || prompt.length() > 8192)
        return control::ApplyResult::failure("not_configured", "Enable a provider with an API key, model and a request of at most 8192 characters");
    const auto initial = service.context(); const auto document = initial["documentId"].toString();
    if (document.isEmpty()) return control::ApplyResult::failure("no_document", "Open a project first");
    if (worker_.joinable()) worker_.join();
    if (state_) state_->alive = false; // drop any old completion queued before this run
    auto state = std::make_shared<State>(); state->events = std::move(events); state->key = std::move(key);
    state->service = &service; state->document = document; state_ = state;
    const auto transport = transport_;
    worker_ = std::thread([state, config, prompt, transport] {
        int calls = 0, edits = 0;
        const auto notify = [state](std::function<void()> callback) {
            juce::MessageManager::callAsync([state, queued = std::move(callback)] { if (state->alive) queued(); });
        };
        const auto safeText = [state](juce::String value) { return value.replace(state->key, "[redacted]").substring(0, 65536); };
        juce::String final;
        try
        {
            Conversation conversation(config, prompt);
            const auto started = std::chrono::steady_clock::now();
            bool completed = false;
            for (int round = 0; round < 12 && ! state->cancelled; ++round)
            {
                if (std::chrono::steady_clock::now() - started > std::chrono::minutes(3)) throw std::runtime_error("Native LLM time budget reached");
                const auto response = transport(config, state->key, conversation.request(), state->cancelled);
                if (state->cancelled) break;
                if (response.error.isNotEmpty()) throw std::runtime_error(response.error.toStdString());
                const auto turn = conversation.receive(response.response);
                if (turn.text.isNotEmpty()) notify([state, value = safeText(turn.text)] { if (state->events.text) state->events.text(value); });
                if (turn.calls.empty()) { completed = true; break; }
                for (const auto& call : turn.calls)
                {
                    if (state->cancelled) break;
                    if (++calls > 32) throw std::runtime_error("Native LLM tool budget reached");
                    struct Pending { std::mutex mutex; std::condition_variable ready; juce::var result; bool started = false, done = false; };
                    auto pending = std::make_shared<Pending>();
                    { const std::lock_guard lock(state->mutex); state->interruptWait = [pending] { pending->ready.notify_all(); }; }
                    // Keys are never in the conversation or tool arguments. Requests stay bound to this document.
                    juce::MessageManager::callAsync([state, pending, call, number = calls] {
                        if (! state->alive || state->cancelled) return;
                        { const std::lock_guard lock(pending->mutex); pending->started = true; }
                        const auto result = executeTool(*state->service, call, state->document, state->runId + "-" + juce::String(number));
                        if (state->events.tool) state->events.tool(operationForTool(call.name), result);
                        { const std::lock_guard lock(pending->mutex); pending->result = result; pending->done = true; }
                        pending->ready.notify_all();
                    });
                    std::unique_lock lock(pending->mutex);
                    while (! pending->done && state->alive && (! state->cancelled || pending->started))
                        pending->ready.wait_for(lock, std::chrono::milliseconds(50));
                    if (! pending->done) break;
                    const auto result = pending->result; lock.unlock();
                    if (result["status"].toString() == "committed") ++edits;
                    conversation.addResult(call, result);
                    const auto code = result["error"]["code"].toString();
                    if (code == "stale_document" || code == "stale_revision" || code == "undo_conflict" || code == "outcome_unknown")
                        throw std::runtime_error("Stopped after a conflict or uncertain outcome. Review action results before continuing.");
                }
            }
            if (state->cancelled) final = "Stopped. Already applied edits remain in the project.";
            else if (! completed) final = "Native LLM turn limit reached. Review applied actions before starting another request.";
            else final = "Request finished.";
        }
        catch (const std::exception& error) { final = "Stopped: " + safeText(juce::String::fromUTF8(error.what())); }
        { const std::lock_guard lock(state->mutex); state->interruptWait = {}; }
        final += " " + juce::String(calls) + " tool requests; " + juce::String(edits) + " applied edits. Changes remain unsaved.";
        state->running = false;
        notify([state, final] { if (state->events.finished) state->events.finished(final); });
    });
    return control::ApplyResult::success();
}
}
