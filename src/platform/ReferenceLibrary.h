#pragma once
// LIB-2: where the reference application keeps its library on each OS. Sonora only reads these folders at run time
// (device presets, docs/ALS_FORMAT.md "Device presets"); nothing in them is ever copied into the repository or the app.
//
//   referenceCoreLibraries():  macOS /Applications/Ableton Live*.app/Contents/App-Resources/Core Library
//                              Windows %ProgramData%\Ableton\Live*\Resources\Core Library      Linux: none
//   referenceUserLibrary():    ~/Music/Ableton/User Library (Windows: Documents\Ableton\User Library)
//   referencePackFolders():    ~/Music/Ableton/Factory Packs and the shared /Users/Shared/Ableton/Factory Packs
//                              (Windows: Documents\Ableton\Factory Packs, %ProgramData%\Ableton\Factory Packs)

#include <juce_core/juce_core.h>

#include <vector>

namespace sonora::platform
{

// Every installed edition, newest name first (Suite / Standard / Intro / Lite, 12 before 11). Existing folders only.
inline std::vector<juce::File> referenceCoreLibraries()
{
    std::vector<juce::File> result;
#if JUCE_MAC
    auto bundles = juce::File("/Applications").findChildFiles(juce::File::findDirectories, false, "Ableton Live*.app");
    bundles.sort();
    for (int i = bundles.size(); --i >= 0;)
        if (const auto library = bundles[i].getChildFile("Contents/App-Resources/Core Library"); library.isDirectory())
            result.push_back(library);
#elif JUCE_WINDOWS
    const auto data = juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory).getChildFile("Ableton");
    auto editions = data.findChildFiles(juce::File::findDirectories, false, "Live*");
    editions.sort();
    for (int i = editions.size(); --i >= 0;)
        if (const auto library = editions[i].getChildFile("Resources/Core Library"); library.isDirectory())
            result.push_back(library);
#endif
    return result;
}

inline juce::File referenceUserLibrary()
{
#if JUCE_WINDOWS
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Ableton/User Library");
#else
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Ableton/User Library");
#endif
}

inline std::vector<juce::File> referencePackFolders()
{
#if JUCE_WINDOWS
    return {juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Ableton/Factory Packs"),
            juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory).getChildFile("Ableton/Factory Packs")};
#else
    return {juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Ableton/Factory Packs"),
            juce::File("/Users/Shared/Ableton/Factory Packs")};
#endif
}

} // namespace sonora::platform
