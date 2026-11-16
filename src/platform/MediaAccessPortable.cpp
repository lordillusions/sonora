#include "platform/MediaAccess.h"

namespace sonora::platform
{

MediaAccess mediaAccess(MediaKind) { return MediaAccess::Granted; }

void requestMediaAccess(MediaKind, std::function<void(bool granted)> done)
{
    if (done) done(true);
}

bool localNetworkPromptExists() { return false; }

} // namespace sonora::platform
