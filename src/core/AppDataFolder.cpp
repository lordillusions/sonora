#include "core/AppDataFolder.h"

#include <mutex>

namespace sonora::core
{
namespace
{
std::mutex& folderLock()
{
    static std::mutex lock;
    return lock;
}

std::string& folderName()
{
    static std::string name = kStableDataFolder;
    return name;
}
} // namespace

bool isValidDataFolderName(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name.front() == '.' || name.front() == ' ' || name.back() == ' ')
        return false;
    for (const char c : name)
        if (c == '/' || c == ':' || c == '\\' || static_cast<unsigned char>(c) < 0x20)
            return false;
    return true;
}

bool setAppDataFolderName(const std::string& name)
{
    if (! isValidDataFolderName(name))
        return false;
    const std::lock_guard<std::mutex> guard(folderLock());
    folderName() = name;
    return true;
}

std::string appDataFolderName()
{
    const std::lock_guard<std::mutex> guard(folderLock());
    return folderName();
}

juce::File appDataDirectory()
{
    auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if defined(__APPLE__)
    base = base.getChildFile("Application Support");
#endif
    return base.getChildFile(juce::String::fromUTF8(appDataFolderName().c_str()));
}
} // namespace sonora::core
