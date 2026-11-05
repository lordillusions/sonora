#pragma once
// COL-8: picture-in-picture camera tiles over the main view's top-right corner (UiContext::collab.videoTiles). This
// person's preview is first and mirrored; each tile shows the name, a mic when it is on and a green frame while they
// talk. The x on the first tile hides the tiles on this machine (the Collaborate panel shows them again).

#include "ui/Widget.h"

#include "include/core/SkImage.h"

#include <map>

namespace sonora::ui
{

class CollabVideoTiles : public Widget
{
public:
    static constexpr float tileW = 192.0f, tileH = 144.0f, gap = 6.0f;
    // Height for `count` tiles in one column.
    static float columnHeight(size_t count) { return count == 0 ? 0.0f : (float) count * (tileH + gap) - gap; }

    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent& e) override;

private:
    struct Cached
    {
        uint64_t serial = 0;
        sk_sp<SkImage> image;
    };
    std::map<int, Cached> images_;
    Rect hideButton_;
};

} // namespace sonora::ui
