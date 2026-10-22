#include "ui/Widget.h"

#include "ui/RootWidget.h"

#include "include/core/SkRRect.h"

#include <unordered_set>

namespace sonora::ui
{

namespace
{
std::unordered_set<const Widget*>& liveWidgets()
{
    static std::unordered_set<const Widget*> set;
    return set;
}
} // namespace

Widget::Widget()
{
    liveWidgets().insert(this);
}

Widget::~Widget()
{
    liveWidgets().erase(this);
}

bool Widget::isAlive(const Widget* w)
{
    return w != nullptr && liveWidgets().count(w) > 0;
}

void Widget::setBounds(const Rect& r)
{
    bounds_ = r;
    layout();
}

void Widget::repaint()
{
    Widget* w = this;
    while (w->parent_ != nullptr)
        w = w->parent_;
    if (auto* root = dynamic_cast<RootWidget*>(w))
        root->markDirty();
}

UiContext& Widget::ctx() const
{
    const Widget* w = this;
    while (w->parent_ != nullptr)
        w = w->parent_;
    return static_cast<const RootWidget*>(w)->context();
}

Rect Widget::boundsInRoot() const
{
    Rect r = bounds_;
    for (const Widget* p = parent_; p != nullptr; p = p->parent_)
        r = r.translated(p->bounds_.x, p->bounds_.y);
    return r;
}

void Widget::paintTree(SkCanvas& canvas, const Theme& theme)
{
    if (! visible || bounds_.isEmpty())
        return;
    canvas.save();
    canvas.translate(bounds_.x, bounds_.y);
    if (cornerRadius > 0)
        canvas.clipRRect(SkRRect::MakeRectXY(localBounds().sk(), cornerRadius, cornerRadius), true);
    else
        canvas.clipRect(localBounds().sk());
    paint(canvas, theme);
    for (auto& c : children_)
        c->paintTree(canvas, theme);
    paintOverChildren(canvas, theme);
    canvas.restore();
}

Widget* Widget::findAt(float x, float y, float& localX, float& localY)
{
    if (! visible || ! bounds_.contains(x, y))
        return nullptr;
    const float lx = x - bounds_.x, ly = y - bounds_.y;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
        if (auto* hit = (*it)->findAt(lx, ly, localX, localY))
            return hit;
    if (! interceptsMouse)
        return nullptr;
    localX = lx;
    localY = ly;
    return this;
}

void Widget::tickTree()
{
    if (! visible)
        return;
    tick();
    for (auto& c : children_)
        c->tickTree();
}

} // namespace sonora::ui
