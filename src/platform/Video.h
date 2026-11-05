#pragma once
// COL-8: camera capture and H.264 for collab video. macOS only (ADR 0011): VideoMac.mm uses AVFoundation and
// VideoToolbox; VideoPortable.cpp says "not available" on Windows and Linux.
//
// Images are opaque BGRA, row after row, as Skia's kBGRA_8888 raster reads them. NAL units travel without start codes.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::platform
{

struct VideoImage
{
    int width = 0, height = 0;
    std::vector<uint32_t> bgra; // width * height pixels
};

bool videoAvailable(); // a camera API and an H.264 codec exist on this OS

class Camera
{
public:
    virtual ~Camera() = default;
    // The default camera at about width x height, at most `fps` frames per second. `onFrame` runs on a capture thread.
    // nullptr: no camera, or no permission (ask with requestMediaAccess first).
    static std::unique_ptr<Camera> open(int width, int height, int fps, std::function<void(VideoImage)> onFrame);
};

class H264Encoder
{
public:
    virtual ~H264Encoder() = default;
    static std::unique_ptr<H264Encoder> create(int width, int height, int fps, int bitrate);
    // One access unit: its NAL units in order. Keyframes start with the SPS and PPS. False when the frame was dropped.
    virtual bool encode(const VideoImage&, bool keyframe, std::vector<std::string>& nals) = 0;
};

class H264Decoder
{
public:
    virtual ~H264Decoder() = default;
    static std::unique_ptr<H264Decoder> create();
    // One access unit in; false until an SPS, a PPS and an IDR frame have arrived, or when the frame was damaged.
    virtual bool decode(const std::vector<std::string>& nals, VideoImage& out) = 0;
};

} // namespace sonora::platform
