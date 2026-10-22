#include "app/AppController.h"
#include "control/SerumLead.h"
#include "control/DeviceControl.h"
#include "plugins/PluginDevice.h"
#include <bit>

namespace sonora::app
{
control::State AppController::controlState()
{
    control::State state;
    state.song = song_.get(); state.documentId = controlDocumentId_;
    if (exporting_) state.busy = "Export owns the audio graph";
    else if (ui_.recording) state.busy = "Finish recording and draining before agent edits";
    else if (editingDevices_ || syncingModel_) state.busy = "Device operation in progress";
    if (editingDevices_ || syncingModel_)
    { state.song = nullptr; state.revision = std::to_string(history_.version()); return state; }
    uint64_t fingerprint = 1469598103934665603ULL;
    const auto hash = [&fingerprint](uint64_t value) { fingerprint = (fingerprint ^ value) * 1099511628211ULL; };
    hash(controlGrantGeneration_);
    if (auto controls = engine_.controls())
        for (size_t index = 0; index < controls->size(); ++index)
        {
            const auto& track = (*controls)[index];
            const float gain = track.volume.load();
            state.liveGains.push_back(gain);
            hash(std::bit_cast<uint32_t>(gain)); hash(std::bit_cast<uint32_t>(track.pan.load()));
            hash(track.active.load()); hash(track.solo.load()); hash(track.armed.load());
            for (const auto& send : track.sends) hash(std::bit_cast<uint32_t>(send.load()));
        }
    state.liveTempo = engine_.transport().tempo(); hash(std::bit_cast<uint64_t>(state.liveTempo));
    // Conservative conflict detection includes hosted editor/MIDI writes without serializing plugin state.
    if (auto* graph = engine_.latestGraph())
        for (const auto& node : graph->nodes)
            for (const auto* device : node.processorForModelDevice)
                if (device)
                    for (size_t p = 0; p < device->parameters().size(); ++p)
                        hash(std::bit_cast<uint32_t>(device->getParameter(static_cast<int>(p))));
    state.revision = std::to_string(history_.version()) + ":" + std::to_string(fingerprint);
    return state;
}
void AppController::initialiseControl()
{
    control::Backend backend;
    backend.state = [this] { return controlState(); };
    backend.apply = [this](model::EditCommand command) { return applyEdit(std::move(command)); };
    backend.undo = [this] {
        const int version = history_.version(); undo();
        return version != history_.version() ? control::ApplyResult::success()
            : control::ApplyResult::failure("undo_conflict", "History could not be undone");
    };
    backend.prepareSerum = [this](model::Device& device, juce::var& report) {
        return control::prepareSerumLead(host_, engine_.sampleRate(), engine_.blockSize(), device, report);
    };
    control::configureDeviceBackend(backend, host_, engine_, [this] { return song_.get(); });
    // Plugin methods may run nested message loops. Keep document/device edits excluded while capturing/preparing.
    const auto capture = backend.captureDevice;
    backend.captureDevice = [this, capture](int track, int device, model::Device& out) {
        const juce::ScopedValueSetter<bool> guard(syncingModel_, true); return capture(track, device, out);
    };
    const auto prepare = backend.prepareParameters;
    backend.prepareParameters = [this, prepare](int track, int device, const auto& writes, auto& before, auto& after, auto& report) {
        const juce::ScopedValueSetter<bool> guard(syncingModel_, true); return prepare(track, device, writes, before, after, report);
    };
    const auto insert = backend.prepareDevice;
    backend.prepareDevice = [this, insert](const auto& id, auto& device, auto& report) {
        const juce::ScopedValueSetter<bool> guard(syncingModel_, true); return insert(id, device, report);
    };
    control_ = std::make_unique<control::ControlService>(std::move(backend));
}
control::ApplyResult AppController::enableLocalControl(bool enabled)
{
    ++controlGrantGeneration_;
    if (! enabled) { controlEndpoint_.disable(); return control::ApplyResult::success(); }
    if (! song_) return control::ApplyResult::failure("no_document", "Open a project before enabling local automation");
    const auto document = controlDocumentId_;
    return controlEndpoint_.enable(control_->instanceId(), document, [this, document](const juce::var& request) {
        if (controlDocumentId_ != document)
            return control::object({{"ok", false}, {"error", control::object({{"code", "stale_document"}, {"message", "Document changed; enable automation again"}})}});
        auto result = control_->execute(request);
        if (onControlActivity) onControlActivity(request["operation"].toString(), result);
        return result;
    });
}
} // namespace sonora::app
