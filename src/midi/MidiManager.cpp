#include "midi/MidiManager.h"
#include "core/InputRouting.h"

#include <algorithm>
#include <cmath>

namespace sonora::midi
{

int placeInBlock(double timestamp, double blockStartTime, int numSamples, double sampleRate)
{
    if (numSamples <= 0)
        return 0;
    const double offset = (timestamp - blockStartTime) * sampleRate;
    return std::clamp((int) std::lround(offset), 0, numSamples - 1);
}

MidiManager::MidiManager() = default;

MidiManager::~MidiManager()
{
    for (auto& in : inputs_)
        in->stop();
}

juce::Array<juce::MidiDeviceInfo> MidiManager::availableInputs() const
{
    return juce::MidiInput::getAvailableDevices();
}

// ----------------------------------------------------------------------------- devices

InputRoles MidiManager::inputRoles(const juce::String& identifier) const
{
    for (const auto& in : inputs_)
        if (in->getIdentifier() == identifier)
            for (const auto& s : slots_)
                if (s.input.load() == in.get())
                    return {s.track.load(), s.remote.load()};
    return {false, false};
}

bool MidiManager::isInputEnabled(const juce::String& identifier) const
{
    const auto r = inputRoles(identifier);
    return r.track || r.remote;
}

void MidiManager::setInputRoles(const juce::String& identifier, const juce::String& name, InputRoles roles)
{
    if (! roles.remote) releaseRemoteHolds(model::midiPortToken(identifier.toStdString()));
    auto it = std::find_if(remembered_.begin(), remembered_.end(), [&](const auto& r) { return r.id == identifier; });
    if (it == remembered_.end())
        remembered_.push_back({identifier, name, roles});
    else
        it->roles = roles, it->name = name.isNotEmpty() ? name : it->name;

    const bool open = std::any_of(inputs_.begin(), inputs_.end(), [&](const auto& in) { return in->getIdentifier() == identifier; });
    const bool wanted = roles.track || roles.remote;

    if (wanted && ! open)
    {
        if (auto in = juce::MidiInput::openDevice(identifier, this))
        {
            for (auto& s : slots_)
                if (s.input.load() == nullptr)
                {
                    s.track = roles.track;
                    s.remote = roles.remote;
                    s.token = model::midiPortToken(identifier.toStdString());
                    s.input = in.get();
                    break;
                }
            in->start();
            inputs_.push_back(std::move(in));
        }
    }
    else if (! wanted && open)
    {
        for (auto it2 = inputs_.begin(); it2 != inputs_.end(); ++it2)
            if ((*it2)->getIdentifier() == identifier)
            {
                (*it2)->stop();
                for (auto& s : slots_)
                    if (s.input.load() == it2->get())
                        s.input = nullptr;
                inputs_.erase(it2);
                break;
            }
    }
    else if (open)
    {
        for (const auto& in : inputs_)
            if (in->getIdentifier() == identifier)
                for (auto& s : slots_)
                    if (s.input.load() == in.get())
                    {
                        s.track = roles.track;
                        s.remote = roles.remote;
                    }
    }
}

void MidiManager::setInputEnabled(const juce::String& identifier, bool enabled)
{
    juce::String name;
    for (const auto& info : availableInputs())
        if (info.identifier == identifier)
            name = info.name;
    setInputRoles(identifier, name, {enabled, enabled});
}

void MidiManager::enableAllInputs()
{
    for (const auto& info : availableInputs())
    {
        // Keep roles the user already chose; devices we have not seen get both roles.
        const auto known = std::find_if(remembered_.begin(), remembered_.end(), [&](const auto& r) { return r.id == info.identifier; });
        setInputRoles(info.identifier, info.name, known != remembered_.end() ? known->roles : InputRoles{true, true});
    }
}

bool MidiManager::saveSettings(const juce::File& file) const
{
    juce::Array<juce::var> arr;
    for (const auto& r : remembered_)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", r.id);
        o->setProperty("name", r.name);
        o->setProperty("track", r.roles.track);
        o->setProperty("remote", r.roles.remote);
        arr.add(juce::var(o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("inputs", arr);
    file.getParentDirectory().createDirectory();
    return file.replaceWithText(juce::JSON::toString(juce::var(root)));
}

bool MidiManager::loadSettings(const juce::File& file)
{
    const auto parsed = juce::JSON::parse(file);
    const auto* inputs = parsed.getProperty("inputs", {}).getArray();
    if (inputs == nullptr)
        return false;
    for (const auto& v : *inputs)
    {
        const InputRoles roles{(bool) v.getProperty("track", true), (bool) v.getProperty("remote", true)};
        setInputRoles(v.getProperty("id", {}).toString(), v.getProperty("name", {}).toString(), roles);
    }
    return true;
}

// ----------------------------------------------------------------------------- mappings

void MidiManager::beginLearn(const ControlTarget& target)
{
    std::lock_guard lock(mappingMutex_);
    learnTarget_ = target;
    learning_.store(true);
}

void MidiManager::cancelLearn()
{
    learning_.store(false);
}

std::vector<Mapping> MidiManager::mappings() const
{
    std::lock_guard lock(mappingMutex_);
    return mappings_;
}

void MidiManager::setMappings(std::vector<Mapping> m)
{
    {
        std::lock_guard lock(mappingMutex_);
        mappings_ = std::move(m);
    }
    releaseRemoteHolds(std::nullopt, true);
    if (onMappingsChanged)
        onMappingsChanged();
}

void MidiManager::addMapping(const Mapping& m)
{
    {
        std::lock_guard lock(mappingMutex_);
        mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(),
                                       [&](const Mapping& o) {
                                           return ((o.keyCode > 0) == (m.keyCode > 0) && o.target == m.target) || o.sameSource(m);
                                       }),
                        mappings_.end());
        mappings_.push_back(m);
    }
    releaseRemoteHolds(std::nullopt, true);
    if (onMappingsChanged)
        onMappingsChanged();
}

void MidiManager::removeMapping(const ControlTarget& target, bool keyboard)
{
    {
        std::lock_guard lock(mappingMutex_);
        mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(), [&](const Mapping& m) {
                            return m.target == target && (m.keyCode > 0) == keyboard;
                        }),
                        mappings_.end());
    }
    releaseRemoteHolds(std::nullopt, true);
    if (onMappingsChanged)
        onMappingsChanged();
}

void MidiManager::releaseRemoteHolds(std::optional<uint32_t> port, bool obsoleteOnly)
{
    std::vector<ControlTarget> released;
    {
        std::lock_guard lock(mappingMutex_);
        for (auto& held : remoteHolds_)
        {
            if (held.released || (port && held.port != *port)) continue;
            if (obsoleteOnly && std::find(mappings_.begin(), mappings_.end(), held.mapping) != mappings_.end()) continue;
            released.push_back(held.mapping.target);
            held.released = true; // retain consumption until the physical release arrives
        }
    }
    for (const auto& target : released)
        postToMessageThread([this, target] { if (onControl) onControl(target, {0.0f, false, 0, false, true}); });
}

// ----------------------------------------------------------------------------- input (MIDI thread)

void MidiManager::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& msg)
{
    bool track = false, remote = false;
    uint32_t port = 0;
    for (const auto& s : slots_)
        if (s.input.load() == source)
        {
            track = s.track.load();
            remote = s.remote.load();
            port = s.token.load();
        }
    handleMessage(msg, track, remote, port);
}

void MidiManager::handleMessage(const juce::MidiMessage& msg, bool trackRole, bool remoteRole, uint32_t port)
{
    if (! trackRole && ! remoteRole)
        return;
    if (onActivity)
        postToMessageThread([this] { if (onActivity) onActivity(); });

    const bool isCC = msg.isController();
    const bool isNoteOn = msg.isNoteOn();
    const bool isNoteOff = msg.isNoteOff(); // includes note-on with velocity zero
    if (! remoteRole && (isCC || isNoteOn || isNoteOff))
    {
        std::lock_guard lock(mappingMutex_);
        const int number = isCC ? msg.getControllerNumber() : msg.getNoteNumber();
        const auto old = std::find_if(remoteHolds_.begin(), remoteHolds_.end(), [&](const RemoteHold& h) {
            return h.released && h.port == port && h.mapping.channel == msg.getChannel()
                   && h.mapping.isNote == (isNoteOn || isNoteOff) && h.mapping.number == number;
        });
        if (old != remoteHolds_.end())
        {
            remoteHolds_.erase(old);
            if (isNoteOff || (isCC && msg.getControllerValue() < 64)) return;
            // A new Track-only press owns its later release; the old Remote hold is already finished.
        }
    }
    if (remoteRole && (msg.isAllNotesOff() || msg.isAllSoundOff())) releaseRemoteHolds(port);
    if (remoteRole && (isCC || isNoteOn || isNoteOff))
    {
        const int ch = msg.getChannel();
        const int number = isCC ? msg.getControllerNumber() : msg.getNoteNumber();
        const int raw = isCC ? msg.getControllerValue() : msg.getVelocity();

        std::optional<Mapping> hit;
        bool learned = false, notify = true;
        {
            std::lock_guard lock(mappingMutex_);
            if (! isNoteOff && learning_.exchange(false))
            {
                Mapping m;
                m.channel = ch;
                m.isNote = isNoteOn;
                m.number = number;
                m.target = learnTarget_;
                mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(),
                                               [&](const Mapping& o) {
                                                   return (o.keyCode == 0 && o.target == m.target) || o.sameSource(m);
                                               }),
                                mappings_.end());
                mappings_.push_back(m);
                learned = true;
            }
            for (const auto& m : mappings_)
                if (m.keyCode == 0 && m.channel == ch && m.isNote == (isNoteOn || isNoteOff) && m.number == number)
                    hit = m;
            const auto held = std::find_if(remoteHolds_.begin(), remoteHolds_.end(), [&](const RemoteHold& h) {
                return h.port == port && h.mapping.channel == ch && h.mapping.isNote == (isNoteOn || isNoteOff)
                       && h.mapping.number == number;
            });
            const bool release = isNoteOff || (isCC && raw < 64);
            if (! release && held != remoteHolds_.end() && held->released
                && (! hit || hit->target.kind != ControlTarget::Kind::SessionClip))
                remoteHolds_.erase(held); // a fresh non-clip press owns its later release
            else if (release && held != remoteHolds_.end()) hit = held->mapping;
            if (hit && hit->target.kind == ControlTarget::Kind::SessionClip && ! learned)
            {
                if (release)
                {
                    if (held != remoteHolds_.end()) { notify = ! held->released; remoteHolds_.erase(held); }
                    else notify = false; // includes release of the note used to learn a mapping
                }
                else if (held == remoteHolds_.end()) remoteHolds_.push_back({*hit, port});
                else if (held->released) { held->mapping = *hit; held->released = false; }
                else if (isCC) notify = false; // momentary CC buttons act on edges, not repeated values
            }
        }

        if (learned)
        {
            releaseRemoteHolds(std::nullopt, true);
            // The message that taught the mapping only creates it; it does not move the control.
            postToMessageThread([this] { if (onMappingsChanged) onMappingsChanged(); });
            return;
        }

        if (hit)
        {
            if (! notify || (isNoteOff && hit->target.kind != ControlTarget::Kind::SessionClip)) return;
            ControlChange change;
            if (hit->isNote)
            {
                change.released = isNoteOff;
                change.trigger = isNoteOn;
                change.value = hit->target.kind == ControlTarget::Kind::SessionClip ? (float) raw / 127.0f : 1.0f;
            }
            else if (hit->mode == MapMode::Absolute)
                change.value = (float) raw / 127.0f;
            else
                change.relative = true, change.ticks = decodeRelative(hit->mode, raw);
            postToMessageThread([this, t = hit->target, change] {
                if (onControl)
                    onControl(t, change);
            });
            return; // mapped messages are consumed, not forwarded to instruments
        }
    }

    if (! trackRole)
        return;
    if (onTrackMessage)
        onTrackMessage(msg);
    const auto scope = fifo_.write(1);
    if (scope.blockSize1 > 0)
    {
        fifoData_[(size_t) scope.startIndex1] = msg;
        fifoPorts_[(size_t) scope.startIndex1] = port;
    }
    else droppedInputEvents.fetch_add(1, std::memory_order_relaxed);
    // else: FIFO full, message dropped (audio thread stalled).
}

// ----------------------------------------------------------------------------- audio thread

void MidiManager::popForAudio(juce::MidiBuffer& dest, int numSamples, double sampleRate, InputBlock* sources)
{
    auto& block = sources ? *sources : scratchBlock_;
    block.clear();
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    // Events that arrived since the previous block start are spread over this block by arrival time.
    const double blockStart = lastPopTime_ > 0.0 ? lastPopTime_ : now - numSamples / sampleRate;
    lastPopTime_ = now;

    const int ready = fifo_.getNumReady();
    if (ready == 0)
        return;
    const auto scope = fifo_.read(ready);
    auto add = [&](int idx) {
        const auto& m = fifoData_[(size_t) idx];
        const int sample = placeInBlock(m.getTimeStamp(), blockStart, numSamples, sampleRate);
        if (block.add(m.getRawData(), m.getRawDataSize(), sample, fifoPorts_[(size_t) idx]))
            dest.addEvent(m.getRawData(), m.getRawDataSize(), sample);
        else droppedInputEvents.fetch_add(1, std::memory_order_relaxed);
    };
    for (int i = 0; i < scope.blockSize1; ++i)
        add(scope.startIndex1 + i);
    for (int i = 0; i < scope.blockSize2; ++i)
        add(scope.startIndex2 + i);
}

} // namespace sonora::midi
