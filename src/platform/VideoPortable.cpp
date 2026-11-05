#include "platform/Video.h"

// COL-8: the camera is macOS only for now (ADR 0011). TODO(COM-7): Media Foundation / V4L2 capture and an H.264 codec.

namespace sonora::platform
{

bool videoAvailable() { return false; }
std::unique_ptr<Camera> Camera::open(int, int, int, std::function<void(VideoImage)>) { return nullptr; }
std::unique_ptr<H264Encoder> H264Encoder::create(int, int, int, int) { return nullptr; }
std::unique_ptr<H264Decoder> H264Decoder::create() { return nullptr; }

} // namespace sonora::platform
