#pragma once
// AGT-1/2: message-thread control contract shared by native workflows and external agents.
#include "core/Edit.h"
#include <juce_core/juce_core.h>
#include <functional>
#include <map>
#include <type_traits>

namespace sonora::control
{
struct Field
{
    juce::Identifier name;
    juce::var value;
    template<class T> Field(const char* key, T item) : name(key)
    {
        if constexpr (std::is_same_v<T, std::string>) value = juce::String(item);
        else value = juce::var(std::move(item));
    }
};
juce::var object(std::initializer_list<Field> fields);

struct ApplyResult
{
    bool ok = false;
    std::string code, message;
    static ApplyResult success() { return {true, {}, {}}; }
    static ApplyResult failure(std::string code, std::string message) { return {false, std::move(code), std::move(message)}; }
};

struct State
{
    const model::Song* song = nullptr;
    std::string documentId, revision, busy;
    std::vector<double> liveGains;
    double liveTempo = 0;
};

struct Backend
{
    std::function<State()> state;
    std::function<ApplyResult(model::EditCommand)> apply;
    std::function<ApplyResult()> undo;
    std::function<ApplyResult(model::Device&, juce::var&)> prepareSerum;
    std::function<juce::var(int trackId, int deviceId)> parameters;
    struct ParameterWrite
    {
        std::string parameterId, name;
        float normalized = 0;
        std::optional<double> value;
        ParameterWrite(std::string id, std::string label, float norm = 0)
            : parameterId(std::move(id)), name(std::move(label)), normalized(norm) {}
    };
    std::function<juce::var()> deviceCatalog;
    std::function<ApplyResult(const std::string&, model::Device&, juce::var&)> prepareDevice;
    // Capture the live before-state and prepare/verify a new opaque state without changing the live instance.
    std::function<ApplyResult(int, int, const std::vector<ParameterWrite>&, model::Device&, model::Device&, juce::var&)> prepareParameters;
    std::function<ApplyResult(int, int, model::Device&)> captureDevice;
};

class ControlService
{
public:
    explicit ControlService(Backend backend);
    // Strict v1 JSON envelope. Mutations require exact target/revision and a unique idempotency key.
    juce::var execute(const juce::var& request);
    // Deliberately bounded built-in grammar, not an LLM; unsupported prompts return an explicit error.
    juce::var submit(const juce::String& prompt);
    juce::var inspect();
    juce::var context(); // lightweight identity/undo status; never serializes plugin state
    const std::string& instanceId() const { return instanceId_; }

private:
    juce::var dispatch(const juce::String& operation, const juce::var& args, const State& before);
    juce::var result(const char* status, juce::var data = {});
    juce::var failure(const std::string& code, const std::string& message);
    Backend backend_;
    std::string instanceId_ = juce::Uuid().toString().toStdString();
    struct Receipt { juce::String body; juce::var result; };
    std::map<std::string, Receipt> receipts_;
    std::string receiptsDocument_, lastUndoRevision_, lastUndoDocument_;
    std::function<ApplyResult()> lastUndoCheck_;
    bool executing_ = false;
};
} // namespace sonora::control
