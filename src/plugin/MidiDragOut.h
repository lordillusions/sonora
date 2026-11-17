#pragma once
// PLG-25a: "Drag MIDI" in a pattern generator's VST3 editor (Sonora Phrase, Pulse): the part as a Standard MIDI File
// (io/midi/MidiFileExport: format 1, 960 ticks per quarter, tempo and 4/4 meter) in the temporary folder, dragged out of
// the editor to the host or the Finder. In Sonora itself the faces write into a clip instead (ui/GeneratorClipUi.h).
// VST3 editors only, message thread.

#include "core/Model.h"
#include "io/midi/MidiFileExport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <vector>

namespace sonora::plugin
{
// `notes` (beats from 0) over `lengthBeats` as "<temp>/Sonora MIDI/<name>.mid" (replaced each time). An empty File and
// `error` on failure.
inline juce::File writeDragMidiFile(const std::vector<model::MidiNote>& notes, double lengthBeats, double tempo,
                                    const std::string& name, std::string& error)
{
    model::Clip clip;
    clip.kind = model::Clip::Kind::Midi;
    clip.name = name;
    clip.currentStart = 0;
    clip.currentEnd = lengthBeats;
    clip.loop = {0.0, lengthBeats, 0.0, true};
    clip.notes = notes;
    const auto bytes = io::midiClipToSmf(clip, tempo, 4, 4);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Sonora MIDI");
    if (! folder.createDirectory())
    {
        error = "Could not create " + folder.getFullPathName().toStdString();
        return {};
    }
    auto legal = juce::File::createLegalFileName(juce::String::fromUTF8(name.c_str()));
    if (legal.isEmpty()) legal = "Sonora";
    const auto file = folder.getChildFile(legal + ".mid");
    if (! file.replaceWithData(bytes.data(), bytes.size()))
    {
        error = "Could not write " + file.getFullPathName().toStdString();
        return {};
    }
    return file;
}

// Writes the file and starts the external drag from `source` (call it from the editor's mouseDrag).
inline bool dragMidiOut(const std::vector<model::MidiNote>& notes, double lengthBeats, double tempo, const std::string& name,
                        juce::Component* source, std::string& error)
{
    const auto file = writeDragMidiFile(notes, lengthBeats, tempo, name, error);
    if (file == juce::File()) return false;
    return juce::DragAndDropContainer::performExternalDragDropOfFiles({file.getFullPathName()}, false, source);
}
} // namespace sonora::plugin
