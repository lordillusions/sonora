#include "platform/Video.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>

namespace
{
// Copies a BGRA pixel buffer (any row padding) into an image.
bool copyPixels(CVPixelBufferRef buffer, sonora::platform::VideoImage& out)
{
    if (buffer == nullptr || CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA) return false;
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return false;
    const auto width = (int) CVPixelBufferGetWidth(buffer), height = (int) CVPixelBufferGetHeight(buffer);
    const auto stride = CVPixelBufferGetBytesPerRow(buffer);
    const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    out.width = width;
    out.height = height;
    out.bgra.resize((size_t) width * (size_t) height);
    for (int y = 0; y < height && base != nullptr; ++y)
        std::memcpy(out.bgra.data() + (size_t) y * (size_t) width, base + (size_t) y * stride, (size_t) width * 4);
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return base != nullptr;
}
} // namespace

@interface SonoraCameraDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
{
@public
    std::function<void(sonora::platform::VideoImage)> onFrame;
    double interval;
    double last;
}
@end

@implementation SonoraCameraDelegate
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection*)connection
{
    (void) output;
    (void) connection;
    const double t = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
    if (t - last < interval * 0.9) return; // the camera's own rate may be higher
    last = t;
    sonora::platform::VideoImage image;
    if (copyPixels(CMSampleBufferGetImageBuffer(sample), image) && onFrame) onFrame(std::move(image));
}
@end

namespace sonora::platform
{

bool videoAvailable() { return true; }

// ------------------------------------------------------------------------------------------------ camera

namespace
{
class CameraMac final : public Camera
{
public:
    AVCaptureSession* session = nil;
    SonoraCameraDelegate* delegate = nil;
    dispatch_queue_t queue = nil;
    ~CameraMac() override
    {
        [session stopRunning];
        SonoraCameraDelegate* d = delegate;
        dispatch_sync(queue, ^{ d->onFrame = nullptr; }); // after the last frame in flight
    }
};
} // namespace

std::unique_ptr<Camera> Camera::open(int width, int height, int fps, std::function<void(VideoImage)> onFrame)
{
    if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] != AVAuthorizationStatusAuthorized) return nullptr;
    AVCaptureDevice* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    if (device == nil) return nullptr;
    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (input == nil) return nullptr;
    auto camera = std::make_unique<CameraMac>();
    camera->session = [[AVCaptureSession alloc] init];
    if ([camera->session canSetSessionPreset:AVCaptureSessionPreset640x480]) camera->session.sessionPreset = AVCaptureSessionPreset640x480;
    if (! [camera->session canAddInput:input]) return nullptr;
    [camera->session addInput:input];
    AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
    output.videoSettings = @{(NSString*) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                             (NSString*) kCVPixelBufferWidthKey: @(width), (NSString*) kCVPixelBufferHeightKey: @(height)};
    output.alwaysDiscardsLateVideoFrames = YES;
    camera->delegate = [[SonoraCameraDelegate alloc] init];
    camera->delegate->onFrame = std::move(onFrame);
    camera->delegate->interval = 1.0 / std::max(1, fps);
    camera->delegate->last = -1.0e9;
    camera->queue = dispatch_queue_create("sonora.collab.camera", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:camera->delegate queue:camera->queue];
    if (! [camera->session canAddOutput:output]) return nullptr;
    [camera->session addOutput:output];
    [camera->session startRunning];
    return camera;
}

// ------------------------------------------------------------------------------------------------ encoder

namespace
{
class EncoderMac final : public H264Encoder
{
public:
    VTCompressionSessionRef session = nullptr;
    int width = 0, height = 0, fps = 15;
    int64_t frame = 0;
    std::vector<std::string>* sink = nullptr;

    ~EncoderMac() override
    {
        if (session)
        {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
    }

    static void output(void* refcon, void*, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample)
    {
        auto* self = static_cast<EncoderMac*>(refcon);
        if (status != noErr || sample == nullptr || self->sink == nullptr) return;
        bool key = true;
        if (CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false); attachments && CFArrayGetCount(attachments) > 0)
        {
            auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
            key = ! CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
        }
        if (key)
        {
            CMFormatDescriptionRef nullableFormat = CMSampleBufferGetFormatDescription(sample);
            if (nullableFormat == nullptr) return;
            const auto format = (CMFormatDescriptionRef _Nonnull) nullableFormat;
            size_t count = 0;
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr, &count, nullptr);
            for (size_t i = 0; i < count; ++i)
            {
                const uint8_t* set = nullptr;
                size_t size = 0;
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &set, &size, nullptr, nullptr) == noErr)
                    self->sink->emplace_back(reinterpret_cast<const char*>(set), size);
            }
        }
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        const size_t length = CMBlockBufferGetDataLength(block);
        std::string avcc(length, '\0');
        if (CMBlockBufferCopyDataBytes(block, 0, length, avcc.data()) != kCMBlockBufferNoErr) return;
        for (size_t at = 0; at + 4 <= avcc.size();) // 4-byte big-endian lengths
        {
            const size_t n = (size_t) (uint8_t) avcc[at] << 24 | (size_t) (uint8_t) avcc[at + 1] << 16 | (size_t) (uint8_t) avcc[at + 2] << 8
                           | (uint8_t) avcc[at + 3];
            at += 4;
            if (n == 0 || at + n > avcc.size()) break;
            self->sink->push_back(avcc.substr(at, n));
            at += n;
        }
    }

    bool encode(const VideoImage& image, bool keyframe, std::vector<std::string>& nals) override
    {
        if (image.width != width || image.height != height || image.bgra.size() != (size_t) width * (size_t) height) return false;
        CVPixelBufferRef pixels = nullptr;
        NSDictionary* attributes = @{(NSString*) kCVPixelBufferIOSurfacePropertiesKey: @{}};
        if (CVPixelBufferCreate(kCFAllocatorDefault, (size_t) width, (size_t) height, kCVPixelFormatType_32BGRA,
                                (__bridge CFDictionaryRef) attributes, &pixels) != kCVReturnSuccess)
            return false;
        CVPixelBufferLockBaseAddress(pixels, 0);
        auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
        const auto stride = CVPixelBufferGetBytesPerRow(pixels);
        for (int y = 0; y < height; ++y)
            std::memcpy(base + (size_t) y * stride, image.bgra.data() + (size_t) y * (size_t) width, (size_t) width * 4);
        CVPixelBufferUnlockBaseAddress(pixels, 0);
        const CMTime pts = CMTimeMake(frame++, fps);
        NSDictionary* options = keyframe ? @{(NSString*) kVTEncodeFrameOptionKey_ForceKeyFrame: @YES} : nil;
        sink = &nals;
        const OSStatus status = VTCompressionSessionEncodeFrame(session, pixels, pts, kCMTimeInvalid, (__bridge CFDictionaryRef) options, nullptr, nullptr);
        if (status == noErr) VTCompressionSessionCompleteFrames(session, pts);
        sink = nullptr;
        CVPixelBufferRelease(pixels);
        return status == noErr && ! nals.empty();
    }
};
} // namespace

std::unique_ptr<H264Encoder> H264Encoder::create(int width, int height, int fps, int bitrate)
{
    auto encoder = std::make_unique<EncoderMac>();
    encoder->width = width;
    encoder->height = height;
    encoder->fps = fps;
    if (VTCompressionSessionCreate(kCFAllocatorDefault, width, height, kCMVideoCodecType_H264, nullptr, nullptr, nullptr,
                                   &EncoderMac::output, encoder.get(), &encoder->session) != noErr)
        return nullptr;
    auto set = [&](CFStringRef key, CFTypeRef value) { VTSessionSetProperty(encoder->session, key, value); };
    set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel);
    set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    set(kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef) @(bitrate));
    set(kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFTypeRef) @(fps * 2));
    set(kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFTypeRef) @(fps));
    VTCompressionSessionPrepareToEncodeFrames(encoder->session);
    return encoder;
}

// ------------------------------------------------------------------------------------------------ decoder

namespace
{
class DecoderMac final : public H264Decoder
{
public:
    std::string sps, pps;
    CMVideoFormatDescriptionRef format = nullptr;
    VTDecompressionSessionRef session = nullptr;
    VideoImage* target = nullptr;
    bool got = false, idrSeen = false;

    ~DecoderMac() override { reset(); }

    void reset()
    {
        if (session)
        {
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
        if (format)
        {
            CFRelease(format);
            format = nullptr;
        }
        idrSeen = false;
    }

    static void output(void* refcon, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime)
    {
        auto* self = static_cast<DecoderMac*>(refcon);
        if (status == noErr && image != nullptr && self->target != nullptr) self->got = copyPixels(image, *self->target);
    }

    bool rebuild()
    {
        reset();
        const uint8_t* sets[2] = {reinterpret_cast<const uint8_t*>(sps.data()), reinterpret_cast<const uint8_t*>(pps.data())};
        const size_t sizes[2] = {sps.size(), pps.size()};
        if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets, sizes, 4, &format) != noErr) return false;
        NSDictionary* destination = @{(NSString*) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
        VTDecompressionOutputCallbackRecord callback{&DecoderMac::output, this};
        return VTDecompressionSessionCreate(kCFAllocatorDefault, format, nullptr, (__bridge CFDictionaryRef) destination, &callback, &session) == noErr;
    }

    bool decode(const std::vector<std::string>& nals, VideoImage& out) override
    {
        std::string newSps = sps, newPps = pps, avcc;
        bool idr = false;
        for (const auto& n : nals)
        {
            if (n.empty()) continue;
            const int type = (uint8_t) n[0] & 0x1f;
            if (type == 7) newSps = n;
            else if (type == 8) newPps = n;
            else if (type == 5 || type == 1)
            {
                idr = idr || type == 5;
                const auto size = (uint32_t) n.size();
                const char length[4] = {(char) (size >> 24), (char) (size >> 16), (char) (size >> 8), (char) size};
                avcc.append(length, 4);
                avcc += n;
            }
        }
        if (! newSps.empty() && ! newPps.empty() && (newSps != sps || newPps != pps || session == nullptr))
        {
            sps = newSps;
            pps = newPps;
            if (! rebuild()) return false;
        }
        if (session == nullptr || avcc.empty()) return false;
        if (! idrSeen && ! idr) return false; // a damaged start: wait for the next keyframe
        idrSeen = true;
        CMBlockBufferRef block = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, avcc.size(), kCFAllocatorDefault, nullptr, 0, avcc.size(),
                                               kCMBlockBufferAssureMemoryNowFlag, &block) != kCMBlockBufferNoErr)
            return false;
        CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
        CMSampleBufferRef sample = nullptr;
        const size_t sampleSize = avcc.size();
        const OSStatus made = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 0, nullptr, 1, &sampleSize, &sample);
        CFRelease(block);
        if (made != noErr) return false;
        target = &out;
        got = false;
        VTDecodeInfoFlags info = 0;
        const OSStatus status = VTDecompressionSessionDecodeFrame(session, sample, 0, nullptr, &info);
        VTDecompressionSessionWaitForAsynchronousFrames(session);
        target = nullptr;
        CFRelease(sample);
        if (status != noErr) idrSeen = false; // damaged: wait for the next keyframe
        return status == noErr && got;
    }
};
} // namespace

std::unique_ptr<H264Decoder> H264Decoder::create() { return std::make_unique<DecoderMac>(); }

} // namespace sonora::platform
