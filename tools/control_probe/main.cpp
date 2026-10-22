#include "control/ControlService.h"
#include "agent/AgentRunner.h"
#include "control/LocalEndpoint.h"
#include "control/SerumLead.h"
#include "control/HostedParameters.h"
#include "control/DeviceControl.h"
#include "engine/AudioEngine.h"
#include "engine/Exporter.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "io/als/AlsTemplates.h"
#include "midi/MidiManager.h"
#include "plugins/PluginDevice.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <csignal>
#include <CoreFoundation/CoreFoundation.h>
#if SONORA_PROBE_UI
#include "app/AssistantDock.h"
#endif

using namespace sonora;
namespace
{
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
}
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::StringArray args(argv + 1, argc - 1);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-control-" + juce::Uuid().toString());
    folder.createDirectory();
    const auto source = folder.getChildFile("Source.als"); std::string error;
    if (! io::writeAlsDom(*io::createNewSetDom(), source, error)) return 1;
    auto loaded = io::loadAls(source); if (! loaded) return 1;
    auto song = std::move(loaded.song);
    plugins::PluginHost host; host.loadCache(); midi::MidiManager midi;
    engine::AudioEngine engine(host, midi); engine.loadSong(*song);
    model::EditHistory history;
    auto document = juce::Uuid().toString().toStdString();
    control::Backend backend;
    backend.state = [&] { control::State s; s.song = song.get(); s.documentId = document;
        s.revision = std::to_string(history.version()); return s; };
    backend.apply = [&](model::EditCommand command) {
        history.perform(*song, std::move(command));
        for (const auto& note : engine.loadSong(*song)) std::fprintf(stderr, "Graph: %s\n", note.c_str());
        return control::ApplyResult::success();
    };
    backend.undo = [&] { history.undo(*song); engine.loadSong(*song); return control::ApplyResult::success(); };
    backend.prepareSerum = [&](model::Device& device, juce::var& report) {
        return control::prepareSerumLead(host, engine.sampleRate(), engine.blockSize(), device, report);
    };
    control::configureDeviceBackend(backend, host, engine, [&] { return song.get(); });
    control::ControlService service(std::move(backend));
    if (args.contains("--keychain"))
    {
        agent::Settings settings(folder.getChildFile("Agent-test.json")); auto config = agent::preset("custom");
        config.baseUrl = "http://127.0.0.1:18081/" + juce::Uuid().toString(); config.model = "fixture"; config.enabled = true;
        const auto dummy = "dummy-test-" + juce::Uuid().toString();
        const auto saved = settings.save(config, dummy); juce::String read;
        const bool stored = saved.ok && settings.readKey(config, read).ok && read == dummy;
        auto other = config; other.baseUrl += "/different";
        const bool isolated = ! settings.hasKey(other);
        const bool noFileKey = ! folder.getChildFile("Agent-test.json").loadFileAsString().contains(dummy);
        const bool removed = settings.removeKey(config).ok && ! settings.hasKey(config);
        std::printf("Keychain: stored/read=%d endpoint-isolated=%d no-file-key=%d removed=%d\n", stored, isolated, noFileKey, removed);
        if (! saved.ok) std::fprintf(stderr, "%s\n", saved.message.c_str());
        return stored && isolated && noFileKey && removed ? 0 : 41;
    }
    if (args.contains("--serve"))
    {
        control::LocalEndpoint endpoint;
        const auto started = endpoint.enable(service.instanceId(), document, [&](const auto& request) { return service.execute(request); });
        if (! started.ok) { std::fprintf(stderr, "%s\n", started.message.c_str()); return 1; }
        std::printf("%s\n", endpoint.registryFile().getFullPathName().toRawUTF8()); std::fflush(stdout);
        std::signal(SIGTERM, interrupt); std::signal(SIGINT, interrupt);
        // A headless process has no NSApplication to run; pump the main CF loop for JUCE callbacks.
        while (! interrupted) CFRunLoopRunInMode(kCFRunLoopDefaultMode, .1, true);
        if (args.contains("--verify-devices-on-exit"))
        {
            auto& track = song->tracks[0]; if (track.devices.size() != 2) return 44;
            auto* graph = engine.latestGraph();
            for (size_t d = 0; d < track.devices.size(); ++d) devices::captureDeviceState(track.devices[d], graph->nodes[0].processorForModelDevice[d]);
            const auto file = folder.getChildFile("McpDevices.als"); if (! io::saveAls(*song, file).ok) return 45;
            auto reloaded = io::loadAls(file); if (! reloaded || reloaded.song->tracks[0].devices.size() != 2) return 46;
            engine.loadSong(*reloaded.song); graph = engine.latestGraph();
            auto* plugin = graph->nodes[0].processorForModelDevice[0]; auto* utility = graph->nodes[0].processorForModelDevice[1];
            if (! plugin || ! utility || std::abs(utility->getParameter(0) - .3f) > 1e-5f) return 47;
            const int volume = plugin->findParameterByHostId(0, "Main Vol");
            if (volume < 0 || std::abs(plugin->getParameter(volume) - .27f) > 1e-5f) return 48;
            std::printf("PASS: MCP native and generic VST3 insertion/removal/edit/undo saved and reloaded\n");
        }
        if (args.contains("--verify-edits-on-exit"))
        {
            if (song->tracks.size() != 5) return 20;
            auto& track = song->tracks.back();
            if (track.arrangementClips[0].notes.size() != 12 || track.arrangementClips[0].notes[0].velocity != 62.5f) return 21;
            auto* processor = engine.latestGraph()->nodes[4].processorForModelDevice[0];
            const int volume = processor->findParameterByHostId(0, "Main Vol");
            if (std::abs(processor->getParameter(volume) - .27f) > 1e-4f) return 22;
            if (! dynamic_cast<plugins::PluginDevice*>(processor)->captureState(track.devices[0])) return 23;
            const auto editedFile = folder.getChildFile("McpEdited.als");
            if (! io::saveAls(*song, editedFile).ok) return 24;
            auto reloaded = io::loadAls(editedFile); if (! reloaded) return 25;
            const auto& notes = reloaded.song->tracks.back().arrangementClips[0].notes;
            if (notes.size() != 12 || notes[0].velocity != 62.5f
                || std::none_of(notes.begin(), notes.end(), [](const auto& n) { return n.pitch == 75 && std::abs(n.start - .5) < 1e-9; })) return 26;
            engine.loadSong(*reloaded.song);
            processor = engine.latestGraph()->nodes[4].processorForModelDevice[0];
            if (std::abs(processor->getParameter(processor->findParameterByHostId(0, "Main Vol")) - .27f) > 1e-4f) return 27;
            std::printf("PASS: MCP MIDI and VST3 edits save/reload verified\n");
        }
        return 0;
    }
#if SONORA_PROBE_UI
    if (args.contains("--native-snapshot"))
    {
        juce::DocumentWindow window("Sonora Assistant verification", juce::Colours::darkgrey, juce::DocumentWindow::allButtons);
        auto* dock = new app::AssistantDock(false); dock->setControlService(service);
        window.setUsingNativeTitleBar(true); window.setContentOwned(dock, false); window.centreWithSize(420, 780); window.setVisible(true);
        const auto image = folder.getChildFile("assistant-native.png");
        bool captured = false;
        juce::Timer::callAfterDelay(500, [&] {
            const bool input = dock->verifyNativeInput() && dock->verifyPanelModes();
            captured = dock->writeSnapshot(image) && input;
            dock->previewNativeConfiguration(true);
            captured = dock->writeSnapshot(folder.getChildFile("agent-control-native-llm.png")) && captured;
            dock->previewNativeConfiguration(false);
            std::printf("Native marked-text / Unicode / Return / Shift+Return: %s\n", input ? "PASS" : "FAIL");
            dock->configure();
            juce::Timer::callAfterDelay(400, [&] {
                captured = dock->writeConfigurationSnapshot(folder.getChildFile("agent-control-settings.png")) && captured;
                dock->closeConfiguration();
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
        });
        juce::MessageManager::getInstance()->runDispatchLoop();
        std::printf("%s\n", image.getFullPathName().toRawUTF8()); return captured ? 0 : 1;
    }
#endif
    if (args.contains("--vital"))
    {
        // A second vendor proves the general path has no Serum-specific parameter IDs or state assumptions.
        const auto types = host.knownPlugins().getTypes();
        const auto found = std::find_if(types.begin(), types.end(), [](const auto& d) { return d.name == "Vital" && d.pluginFormatName == "VST3"; });
        if (found == types.end()) return 30;
        juce::String pluginError;
        auto instance = host.formats().createPluginInstance(*found, engine.sampleRate(), engine.blockSize(), pluginError);
        if (! instance) return 31;
        plugins::PluginDevice initial(std::move(instance)); initial.prepare(engine.sampleRate(), engine.blockSize());
        juce::String parameterId, parameterName; float oldValue = 0;
        for (auto* p : initial.instance().getParameters())
            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(p);
                hosted && p->getName(256).containsIgnoreCase("volume") && p->isAutomatable() && ! p->isDiscrete()
                    && (static_cast<int>(p->getCategory()) >> 16) != 2)
            { parameterId = hosted->getParameterID(); parameterName = p->getName(256); oldValue = p->getValue(); break; }
        if (parameterId.isEmpty()) return 32;
        model::Device device; device.kind = model::DeviceKind::PluginVst3; device.type = "PluginDevice";
        device.displayName = found->name.toStdString(); device.plugin.emplace();
        device.plugin->name = found->name.toStdString(); device.plugin->vendor = found->manufacturerName.toStdString(); device.plugin->isInstrument = true;
        if (! initial.captureState(device)) return 33;
        model::Song staged = *song; int endId = staged.nextPointeeId;
        auto track = io::createMidiTrack(staged, "Generic Vital verification", 16, endId); staged.nextPointeeId = endId;
        model::allocateDeviceIds(staged, device); device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
        track.devices.push_back(device); const int trackId = track.id, deviceId = device.id;
        history.perform(*song, model::insertTracks({track}, staged.nextPointeeId, "Verification fixture"));
        song->nextDeviceId = staged.nextDeviceId; engine.loadSong(*song);
        const float requested = oldValue > .5f ? .25f : .75f;
        const auto receipt = service.execute(control::object({{"apiVersion", "1.0"}, {"operation", "devices.parameters.set"},
            {"target", control::object({{"instanceId", service.instanceId()}, {"documentId", document}})},
            {"expectedRevision", service.inspect()["revision"]}, {"idempotencyKey", "vital-parameters"},
            {"arguments", control::object({{"trackId", trackId}, {"deviceId", deviceId}, {"parameters", juce::Array<juce::var>{
                control::object({{"parameterId", parameterId}, {"name", parameterName}, {"normalized", requested}})}}})}}));
        if (! static_cast<bool>(receipt["ok"])) { std::fprintf(stderr, "%s\n", juce::JSON::toString(receipt).toRawUTF8()); return 34; }
        const auto saved = folder.getChildFile("VitalEdited.als"); if (! io::saveAls(*song, saved).ok) return 35;
        auto reload = io::loadAls(saved); if (! reload) return 36; engine.loadSong(*reload.song);
        const auto readback = [&]() {
            const auto values = control::describeHostedParameters(engine.latestGraph()->nodes[4].processorForModelDevice[0]);
            for (const auto& value : *values.getArray()) if (value["parameterId"].toString() == parameterId) return static_cast<float>(value["normalized"]);
            return -1.0f;
        };
        if (std::abs(readback() - requested) > 1e-4f) return 37;
        if (! static_cast<bool>(service.submit("undo")["ok"]) || std::abs(readback() - oldValue) > 1e-4f) return 38;
        if (! io::saveAls(*song, folder.getChildFile("VitalUndone.als")).ok) return 39;
        history.redo(*song); engine.loadSong(*song); if (std::abs(readback() - requested) > 1e-4f) return 40;
        std::printf("PASS: generic %s %s VST3 parameter '%s' [%s], %.6f -> %.6f, save/reload and undo/redo.\n",
            found->name.toRawUTF8(), found->version.toRawUTF8(), parameterName.toRawUTF8(), parameterId.toRawUTF8(),
            static_cast<double>(oldValue), static_cast<double>(requested)); return 0;
    }
    juce::var nativeReceipt;
    if (args.contains("--agent-runner"))
    {
        auto config = agent::preset("openai"); config.enabled = true;
        if (args.contains("--chat")) config = agent::preset("gemini");
        if (args.contains("--anthropic")) config = agent::preset("anthropic");
        config.enabled = true;
        const int httpIndex = args.indexOf("--agent-http");
        if (httpIndex >= 0 && httpIndex + 1 < args.size()) { config.baseUrl = args[httpIndex + 1]; config.provider = "custom"; config.model = "fixture"; }
        int round = 0, toolEvents = 0; bool finished = false; juce::String revision, completion;
        agent::Transport transport = [&](const auto&, const auto&, const juce::var& payload, const auto&) -> agent::HttpResult {
            if (round > 0)
            {
                const auto items = config.protocol == agent::Protocol::Responses ? payload["input"] : payload["messages"];
                const auto last = items[items.size() - 1];
                const auto encoded = config.protocol == agent::Protocol::Responses ? last["output"].toString()
                    : config.protocol == agent::Protocol::Anthropic ? last["content"][0]["content"].toString() : last["content"].toString();
                revision = juce::JSON::parse(encoded)["revision"].toString();
            }
            juce::String name; juce::var arguments = control::object({});
            if (round == 0) name = "sonora_project_inspect";
            if (round == 1) name = "sonora_capabilities_get";
            if (round == 2)
            {
                name = "sonora_serum_supersaw";
                arguments = control::object({{"root", "D#"}, {"mode", "minor"}, {"bars", 4}, {"startBeat", 0}, {"expectedRevision", revision}});
            }
            const auto id = "fixture-" + juce::String(round++);
            if (config.protocol == agent::Protocol::Responses)
            {
                const auto item = name.isEmpty()
                    ? control::object({{"type", "message"}, {"role", "assistant"}, {"content", juce::Array<juce::var>{control::object({{"type", "output_text"}, {"text", "Created and verified the requested progression."}})}}})
                    : control::object({{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", juce::JSON::toString(arguments, true)}});
                return {control::object({{"status", "completed"}, {"output", juce::Array<juce::var>{item}}}), {}};
            }
            if (config.protocol == agent::Protocol::Anthropic)
            {
                const auto item = name.isEmpty() ? control::object({{"type", "text"}, {"text", "Created the progression."}})
                    : control::object({{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", arguments}});
                return {control::object({{"stop_reason", name.isEmpty() ? "end_turn" : "tool_use"}, {"content", juce::Array<juce::var>{item}}}), {}};
            }
            auto message = control::object({{"role", "assistant"}, {"content", "Created the progression."}});
            if (name.isNotEmpty()) message.getDynamicObject()->setProperty("tool_calls", juce::Array<juce::var>{control::object({
                {"id", id}, {"type", "function"}, {"function", control::object({{"name", name}, {"arguments", juce::JSON::toString(arguments, true)}})}})});
            return {control::object({{"choices", juce::Array<juce::var>{control::object({{"finish_reason", name.isEmpty() ? "stop" : "tool_calls"}, {"message", message}})}}}), {}};
        };
        if (httpIndex >= 0) transport = agent::post;
        agent::Runner runner(std::move(transport));
        agent::Runner::Events events;
        events.tool = [&](const juce::String& operation, const juce::var& result) {
            ++toolEvents; if (operation == "workflow.serum_supersaw") nativeReceipt = result.clone();
        };
        events.finished = [&](const juce::String& outcome) { completion = outcome; std::printf("Native runner: %s\n", outcome.toRawUTF8()); finished = true; };
        const auto started = runner.start(service, config, "dummy-fixture-key", "Please make me a supersaw lead chord progression with Serum 2, in D# minor, with 4 bars", std::move(events));
        auto timerActive = std::make_shared<std::atomic<bool>>(true);
        if (args.contains("--agent-cancel")) juce::Timer::callAfterDelay(100, [timerActive, &runner] { if (*timerActive) runner.cancel(); });
        if (args.contains("--agent-document-change")) juce::Timer::callAfterDelay(100, [timerActive, &document] { if (*timerActive) document = juce::Uuid().toString().toStdString(); });
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 80000;
        while (started.ok && ! finished && juce::Time::getMillisecondCounterHiRes() < deadline) CFRunLoopRunInMode(kCFRunLoopDefaultMode, .01, true);
        *timerActive = false;
        if (args.contains("--agent-cancel") || args.contains("--agent-document-change") || args.contains("--agent-error"))
        {
            const bool stopped = started.ok && finished && song->tracks.size() == 4 && ! history.canUndo() && completion.startsWith("Stopped");
            std::printf("Native stopped-request guard: %s\n", stopped ? "PASS" : "FAIL"); return stopped ? 0 : 43;
        }
        if (! started.ok || ! finished || toolEvents != 3 || ! static_cast<bool>(nativeReceipt["ok"]))
        { runner.cancel(); std::fprintf(stderr, "Native runner acceptance failed\n"); return 42; }
        std::printf("PASS: native %s tool loop reached real Serum through the shared API.\n", agent::protocolName(config.protocol).toRawUTF8());
    }
    const auto receipt = nativeReceipt.isVoid() ? service.submit("Please make me a supersaw lead chord progression with Serum 2, in D# minor, with 4 bars") : nativeReceipt;
    std::printf("%s\n", juce::JSON::toString(receipt, false).toRawUTF8());
    if (! static_cast<bool>(receipt["ok"])) return 1;
    if (song->tracks.size() != 5 || song->tracks.back().arrangementClips[0].notes.size() != 12) return 2;
    auto* graph = engine.latestGraph();
    if (! graph || ! dynamic_cast<plugins::PluginDevice*>(graph->nodes[4].processorForModelDevice[0])) return 3;
    juce::AudioBuffer<float> flush(2, engine.blockSize());
    for (int block = 0; block < 4; ++block) engine.renderBlock(flush.getArrayOfWritePointers(), 2, engine.blockSize());
    const auto* instrument = graph->nodes[4].processorForModelDevice[0];
    const int unison = instrument->findParameterByHostId(1000024, "A Unison");
    if (unison < 0 || std::abs(instrument->getParameter(unison) - .4f) > 1e-4f)
    { std::fprintf(stderr, "Restored unison readback: index=%d value=%.6f\n", unison, static_cast<double>(instrument->getParameter(unison))); return 3; }
    const auto preset = song->tracks.back().devices[0].plugin->state;
    const auto saved = folder.getChildFile("Supersaw.als");
    const auto save = io::saveAls(*song, saved);
    if (! save.ok) { std::fprintf(stderr, "save: %s\n", save.error.c_str()); return 4; }
    auto reload = io::loadAls(saved);
    if (! reload || reload.song->tracks.size() != 5 || reload.song->tracks.back().devices[0].plugin->state != preset
        || reload.song->tracks.back().arrangementClips[0].notes.size() != 12) return 5;
    engine.loadSong(*reload.song);
    const auto* restored = engine.latestGraph()->nodes[4].processorForModelDevice[0];
    for (const auto& parameter : *receipt["data"]["parameters"].getArray())
    {
        const int index = restored->findParameterByHostId(parameter["parameterId"].toString().getIntValue(), parameter["name"].toString().toStdString());
        if (index < 0 || std::abs(restored->getParameter(index) - static_cast<float>(parameter["normalized"])) > 1e-4f) return 5;
    }
    engine::ExportOptions options; options.startBeat = 0; options.endBeat = 16; options.tailSeconds = .5;
    options.destination = folder.getChildFile("Supersaw.wav"); options.bitDepth = 32; options.dither = false;
    if (! engine.beginOffline()) return 6;
    engine::Exporter exporter(engine, *reload.song);
    std::atomic<float> progress{0}; std::atomic<bool> cancel{false};
    const auto render = exporter.run(options, progress, cancel); engine.endOffline();
    if (! render.ok || ! std::isfinite(render.samplePeakDb) || render.samplePeakDb < -60 || render.samplePeakDb > 0) return 7;
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> audio(formats.createReaderFor(options.destination));
    if (! audio || audio->numChannels != 2 || std::abs(static_cast<double>(audio->lengthInSamples) - render.seconds * audio->sampleRate) > 1) return 7;
    double barEnergy[4]{};
    juce::AudioBuffer<float> samples(2, 4096);
    for (int64_t position = 0; position < audio->lengthInSamples; position += 4096)
    {
        const int count = static_cast<int>(std::min<int64_t>(4096, audio->lengthInSamples - position));
        if (! audio->read(&samples, 0, count, position, true, true)) return 7;
        for (int i = 0; i < count; ++i)
            for (int ch = 0; ch < 2; ++ch)
            {
                const double sample = samples.getSample(ch, i);
                if (! std::isfinite(sample)) return 7;
                const int bar = static_cast<int>((static_cast<double>(position + i) / audio->sampleRate) / (240 / song->tempo));
                if (bar < 4) barEnergy[bar] += sample * sample;
            }
    }
    for (const double energy : barEnergy) if (energy < .01) return 7;
    // No model or live-control changes intervened, so guarded undo must remove only the newly authored track.
    if (! static_cast<bool>(service.submit("undo")["ok"]) || song->tracks.size() != 4) return 8;
    history.redo(*song);
    if (song->tracks.size() != 5 || song->tracks.back().devices[0].plugin->state != preset) return 9;
    const auto undoSaved = folder.getChildFile("AfterUndoRedo.als");
    if (! io::saveAls(*song, undoSaved).ok) return 10;
    engine.loadSong(*song);
    const auto request = [&](const char* operation, juce::var arguments, const char* key) {
        return control::object({{"apiVersion", "1.0"}, {"operation", operation}, {"arguments", arguments},
            {"target", control::object({{"instanceId", service.instanceId()}, {"documentId", document}})},
            {"expectedRevision", service.inspect()["revision"]}, {"idempotencyKey", key}});
    };
    const int trackId = song->tracks.back().id, clipId = song->tracks.back().arrangementClips[0].uid;
    auto noteArgs = control::object({{"commands", juce::Array<juce::var>{
        control::object({{"type", "notes.update"}, {"trackId", trackId}, {"clipId", clipId},
            {"updates", juce::Array<juce::var>{control::object({{"noteIndex", 0}, {"velocity", 62.5}, {"durationBeats", 2.5}, {"probability", .75}})}}}),
        control::object({{"type", "notes.add"}, {"trackId", trackId}, {"clipId", clipId},
            {"notes", juce::Array<juce::var>{control::object({{"pitch", 75}, {"startBeat", .5}, {"durationBeats", 1.25}, {"velocity", 77}})}}}),
        control::object({{"type", "notes.delete"}, {"trackId", trackId}, {"clipId", clipId}, {"noteIndices", juce::Array<juce::var>{1}}})}}});
    auto edited = service.execute(request("edits.apply", noteArgs, "probe-notes"));
    if (! static_cast<bool>(edited["ok"]) || song->tracks.back().arrangementClips[0].notes.size() != 12) return 11;
    if (! static_cast<bool>(service.submit("undo")["ok"]) || std::abs(song->tracks.back().arrangementClips[0].notes[0].velocity - 88.0f) > 1e-5f) return 12;
    history.redo(*song); engine.loadSong(*song);
    auto* live = dynamic_cast<plugins::PluginDevice*>(engine.latestGraph()->nodes[4].processorForModelDevice[0]);
    // Prove undo captures actual editor state rather than the older model state (.35).
    live->setParameter(live->findParameterByHostId(0, "Main Vol"), .31f);
    for (int block = 0; block < 4; ++block) engine.renderBlock(flush.getArrayOfWritePointers(), 2, engine.blockSize());
    auto& deviceModel = song->tracks.back().devices[0];
    model::PluginParam storedVolume; storedVolume.parameterId = 0; storedVolume.name = "Main Vol";
    storedVolume.value = .35; storedVolume.automationTargetId = song->nextPointeeId++;
    deviceModel.pluginParams.push_back(storedVolume);
    auto xml = juce::parseXML(juce::String(*deviceModel.sourceXml));
    auto* list = xml->getChildByName("ParameterList"); if (! list) list = xml->createNewChildElement("ParameterList");
    auto* pp = list->createNewChildElement("PluginFloatParameter"); pp->setAttribute("Id", 0);
    pp->createNewChildElement("ParameterId")->setAttribute("Value", 0);
    pp->createNewChildElement("ParameterName")->setAttribute("Value", "Main Vol");
    auto* pv = pp->createNewChildElement("ParameterValue"); pv->createNewChildElement("Manual")->setAttribute("Value", .35);
    pv->createNewChildElement("AutomationTarget")->setAttribute("Id", storedVolume.automationTargetId);
    deviceModel.sourceXml = std::make_shared<const std::string>(xml->toString().toStdString());
    const int deviceId = song->tracks.back().devices[0].id;
    auto parameterArgs = control::object({{"trackId", trackId}, {"deviceId", deviceId}, {"parameters", juce::Array<juce::var>{
        control::object({{"parameterId", "0"}, {"name", "Main Vol"}, {"normalized", .27}}),
        control::object({{"parameterId", "1000024"}, {"name", "A Unison"}, {"normalized", .2}})}}});
    auto parameterRequest = request("devices.parameters.set", parameterArgs, "probe-parameters");
    const auto parameterReceipt = service.execute(parameterRequest);
    if (! static_cast<bool>(parameterReceipt["ok"]))
    { std::fprintf(stderr, "Parameter edit: %s\n", juce::JSON::toString(parameterReceipt).toRawUTF8()); return 13; }
    if (! static_cast<bool>(service.execute(parameterRequest)["ok"])) return 14;
    // Save can refresh source XML identities; the older command must still undo correctly afterward.
    const auto committedFile = folder.getChildFile("ParametersCommitted.als");
    if (! io::saveAls(*song, committedFile).ok) return 14;
    const auto committedReload = io::loadAls(committedFile);
    if (! committedReload || committedReload.song->tracks.back().devices[0].pluginParams.empty()
        || std::abs(committedReload.song->tracks.back().devices[0].pluginParams[0].value - .27) > 1e-4) return 14;
    if (! static_cast<bool>(service.submit("undo")["ok"])) return 15;
    auto* undone = engine.latestGraph()->nodes[4].processorForModelDevice[0];
    if (std::abs(undone->getParameter(undone->findParameterByHostId(0, "Main Vol")) - .31f) > 1e-4f) return 16;
    const auto undoneFile = folder.getChildFile("ParametersUndone.als");
    if (! io::saveAls(*song, undoneFile).ok) return 16;
    auto undoneReload = io::loadAls(undoneFile); if (! undoneReload) return 16;
    if (undoneReload.song->tracks.back().devices[0].pluginParams.empty()
        || std::abs(undoneReload.song->tracks.back().devices[0].pluginParams[0].value - .31) > 1e-4) return 16;
    engine.loadSong(*undoneReload.song); undone = engine.latestGraph()->nodes[4].processorForModelDevice[0];
    if (std::abs(undone->getParameter(undone->findParameterByHostId(0, "Main Vol")) - .31f) > 1e-4f) return 16;
    history.redo(*song); engine.loadSong(*song);
    const auto editedFile = folder.getChildFile("Edited.als");
    if (! io::saveAls(*song, editedFile).ok) return 17;
    auto editedReload = io::loadAls(editedFile); if (! editedReload) return 18;
    if (editedReload.song->tracks.back().devices[0].pluginParams.empty()
        || std::abs(editedReload.song->tracks.back().devices[0].pluginParams[0].value - .27) > 1e-4) return 18;
    const auto& editedNotes = editedReload.song->tracks.back().arrangementClips[0].notes;
    if (editedNotes.size() != 12 || editedNotes[0].velocity != 62.5f || editedNotes[0].duration != 2.5
        || editedNotes[0].probability != .75f || editedNotes[2].pitch != 75) return 18;
    engine.loadSong(*editedReload.song); const auto* finalPlugin = engine.latestGraph()->nodes[4].processorForModelDevice[0];
    if (std::abs(finalPlugin->getParameter(finalPlugin->findParameterByHostId(0, "Main Vol")) - .27f) > 1e-4f
        || std::abs(finalPlugin->getParameter(finalPlugin->findParameterByHostId(1000024, "A Unison")) - .2f) > 1e-4f) return 19;
    std::printf("PASS: general MIDI and arbitrary VST3 API edits, captured-live undo, redo and edited/undone save/reload.\n%s\n", editedFile.getFullPathName().toRawUTF8());
    std::printf("PASS: real Serum VST3, 12 notes, 16 beats, state save/reload, finite non-silent render %.2f dBFS / %.2f s, undo/redo.\n%s\n%s\n",
        static_cast<double>(render.samplePeakDb), render.seconds, saved.getFullPathName().toRawUTF8(), options.destination.getFullPathName().toRawUTF8());
    return 0;
}
