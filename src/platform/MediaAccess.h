#pragma once
// COL-7/8: permission to use the microphone and the camera (macOS privacy prompts; elsewhere always granted).
// MediaAccessMac.mm asks AVFoundation; MediaAccessPortable.cpp grants (Windows and Linux have no per-app prompt
// that a program must trigger).

#include <functional>

namespace sonora::platform
{

enum class MediaKind
{
    Microphone,
    Camera,
};

enum class MediaAccess
{
    Granted,
    Denied,        // the person said no, or a policy forbids it: only System Settings can change it
    NotDetermined, // never asked: request() shows the system prompt
};

MediaAccess mediaAccess(MediaKind);
// Asks when not determined yet; `done(granted)` runs on the message thread.
void requestMediaAccess(MediaKind, std::function<void(bool granted)> done);

// COM-15: whether the system asks before an app reaches devices on the local network (macOS 15 and later). There is
// no API to ask or to read the answer: the prompt shows the first time collab traffic reaches the LAN.
bool localNetworkPromptExists();

} // namespace sonora::platform
