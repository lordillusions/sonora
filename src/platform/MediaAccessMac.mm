#include "platform/MediaAccess.h"

#include <juce_events/juce_events.h>

#import <AVFoundation/AVFoundation.h>

namespace sonora::platform
{
namespace
{
AVMediaType typeOf(MediaKind kind) { return kind == MediaKind::Camera ? AVMediaTypeVideo : AVMediaTypeAudio; }
} // namespace

MediaAccess mediaAccess(MediaKind kind)
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:typeOf(kind)])
    {
        case AVAuthorizationStatusAuthorized: return MediaAccess::Granted;
        case AVAuthorizationStatusNotDetermined: return MediaAccess::NotDetermined;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted: return MediaAccess::Denied;
    }
    return MediaAccess::Denied;
}

void requestMediaAccess(MediaKind kind, std::function<void(bool granted)> done)
{
    const auto now = mediaAccess(kind);
    if (now != MediaAccess::NotDetermined)
    {
        if (done) done(now == MediaAccess::Granted);
        return;
    }
    auto callback = std::make_shared<std::function<void(bool)>>(std::move(done));
    [AVCaptureDevice requestAccessForMediaType:typeOf(kind)
                             completionHandler:^(BOOL granted) {
                                 juce::MessageManager::callAsync([callback, granted] {
                                     if (*callback) (*callback)(granted == YES);
                                 });
                             }];
}

} // namespace sonora::platform
