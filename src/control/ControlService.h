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

// AGT-7e: one control index (tracks, then returns, then main) of live mixer values (engine atomics in the app).
struct LiveMixer
{
    double volume = 1.0, pan = 0.0;
    bool active = true, solo = false, armed = false;
    int monitoring = 1; // 0 In, 1 Auto, 2 Off
    std::vector<double> sends;
};

struct State
{
    const model::Song* song = nullptr;
    std::string documentId, revision, busy;
    std::vector<double> liveGains;
    double liveTempo = 0;
    std::vector<LiveMixer> liveMixer; // empty when the backend has no live mixer: the model values are reported
    std::string path;                 // current file, empty for an untitled set
    bool untitled = false, dirty = false;
    struct History { bool canUndo = false, canRedo = false; std::string undoLabel, redoLabel; } history;
};

// AGT-7e: a live action (no undo, no revision guard). The service validates and normalises the arguments (track IDs are
// resolved to `trackIndex`, a control index) and the backend performs it, filling `receipt` with previous/new values.
using LiveHandler = std::function<ApplyResult(const juce::var& arguments, juce::var& receipt)>;

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
        std::vector<int> path; // nested native device: chain, child, chain, child... below the top-level rack (empty: the device itself)
        ParameterWrite(std::string id, std::string label, float norm = 0)
            : parameterId(std::move(id)), name(std::move(label)), normalized(norm) {}
    };
    std::function<juce::var()> deviceCatalog;
    std::function<ApplyResult(const std::string&, model::Device&, juce::var&)> prepareDevice;
    // Capture the live before-state and prepare/verify a new opaque state without changing the live instance.
    std::function<ApplyResult(int, int, const std::vector<ParameterWrite>&, model::Device&, model::Device&, juce::var&)> prepareParameters;
    std::function<ApplyResult(int, int, model::Device&)> captureDevice;
    // AGT-7e: copy live plug-in/mixer state into the model before structural staging (the UI does this before such edits).
    std::function<void()> syncModel;
    // AGT-7e: transport, recording, export and plug-in scan state for status.get ({} members are omitted).
    std::function<juce::var()> status;
    // AGT-7e: live actions by operation name ("mixer.set", "transport.set"...). Only present handlers are advertised.
    std::map<std::string, LiveHandler> live;
    // AGT-7e: guarded general history step. Returns the labels it undid/redid through `label`.
    std::function<ApplyResult(bool undo, std::string& label)> historyStep;
    // AGT-7e: audio import (mirrors the sample-drop path). `paths` are absolute, existing, accepted audio files; the hook
    // places them as one undoable edit and fills `receipt` (created tracks and clips).
    std::function<ApplyResult(const std::vector<std::string>& paths, int trackId, double startBeat, int scene, juce::var& receipt)> importAudio;
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
    juce::var liveAction(const juce::String& operation, const juce::var& args, const State& before);
    juce::var capabilities();
    // AGT-7d: public automation commands name their target ("volume", "device" + parameterId...); returns the op form.
    juce::var automationTarget(const model::Song& song, const juce::var& item);
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
