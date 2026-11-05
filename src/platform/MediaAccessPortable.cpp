#include "platform/MediaAccess.h"

namespace sonora::platform
{

MediaAccess mediaAccess(MediaKind) { return MediaAccess::Granted; }

void requestMediaAccess(MediaKind, std::function<void(bool granted)> done)
{
    if (done) done(true);
}

} // namespace sonora::platform
