#include "ui/SampleFiles.h"

#include "io/als/AlsReader.h"
#include "ui/UiContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::ui
{

namespace
{
void refresh(UiContext& ctx)
{
    if (ctx.layoutChanged) ctx.layoutChanged();
    if (ctx.requestRepaint) ctx.requestRepaint();
}

const model::Clip* audioClip(const UiContext& ctx, const model::ClipLocation& location)
{
    const auto* clip = ctx.song ? model::findClip(*ctx.song, location) : nullptr;
    return clip && clip->kind == model::Clip::Kind::Audio ? clip : nullptr;
}
} // namespace

std::string clipSamplePath(const UiContext& ctx, const model::ClipLocation& location)
{
    const auto* clip = audioClip(ctx, location);
    return clip ? io::resolveSamplePath(*ctx.song, clip->sample) : std::string{};
}

bool showInBrowser(UiContext& ctx, const std::string& path)
{
    if (path.empty() || ! juce::File(juce::String::fromUTF8(path.c_str())).exists())
    {
        ctx.statusText = "The sample file is missing";
        if (ctx.requestRepaint) ctx.requestRepaint();
        return false;
    }
    ctx.browserReveal = path;
    ctx.browserVisible = true;
    refresh(ctx);
    return true;
}

bool showSimilarFiles(UiContext& ctx, const std::string& path, const model::ClipLocation& clip)
{
    if (path.empty() || ! ctx.findSimilarSamples)
    {
        if (path.empty()) ctx.statusText = "The sample file is missing";
        return false;
    }
    ctx.browserVisible = true;
    ctx.findSimilarSamples(path, clip);
    refresh(ctx);
    return true;
}

void leaveSimilarFiles(UiContext& ctx)
{
    if (ctx.cancelSimilarSamples) ctx.cancelSimilarSamples();
    const auto revision = ctx.similarSamples.revision;
    ctx.similarSamples = {};
    ctx.similarSamples.revision = revision + 1;
    if (ctx.requestRepaint) ctx.requestRepaint();
}

bool replaceClipSampleWith(UiContext& ctx, const model::ClipLocation& location, const std::string& path)
{
    if (! audioClip(ctx, location) || ! ctx.replaceClipSample || ctx.recording || ctx.recordingRequested || path.empty()) return false;
    ctx.replaceClipSample(location, path);
    return true;
}

namespace
{
bool canReplace(const UiContext& ctx, const model::ClipLocation& location)
{
    const auto* track = ctx.trackAt(location.track);
    return audioClip(ctx, location) && static_cast<bool>(ctx.replaceClipSample) && ! ctx.recording && ! ctx.recordingRequested
           && track != nullptr && ! track->frozen;
}
} // namespace

bool beginSampleSwap(UiContext& ctx, const model::ClipLocation& location)
{
    if (! canReplace(ctx, location)) return false;
    ctx.sampleSwap = {true, location};
    ctx.browserVisible = true;
    const auto path = clipSamplePath(ctx, location);
    const bool similar = ! path.empty() && showSimilarFiles(ctx, path, location);
    const auto* clip = audioClip(ctx, location);
    ctx.statusText = std::string(path.empty() ? "The sample file is missing. " : "")
                   + "Double-click a sample in the browser (any folder) to put it into \u201C" + (clip ? clip->name : std::string{})
                   + "\u201D; Escape ends";
    if (! similar) refresh(ctx);
    else if (ctx.requestRepaint) ctx.requestRepaint();
    return true;
}

void endSampleSwap(UiContext& ctx)
{
    if (! ctx.sampleSwap.active) return;
    ctx.sampleSwap = {};
    if (ctx.requestRepaint) ctx.requestRepaint();
}

const model::Clip* sampleSwapClip(const UiContext& ctx)
{
    if (! ctx.sampleSwap.active) return nullptr;
    const auto* clip = audioClip(ctx, ctx.sampleSwap.clip);
    return clip != nullptr && clip->uid == ctx.sampleSwap.clip.uid ? clip : nullptr;
}

const char* sampleScopeShortLabel(model::SampleScope scope)
{
    switch (scope)
    {
        case model::SampleScope::Clip: return "Clip";
        case model::SampleScope::Track: return "Track";
        case model::SampleScope::Set: return "Set";
    }
    return "";
}

std::vector<MenuAction> sampleFileActions(UiContext& ctx, model::ClipLocation location)
{
    if (! audioClip(ctx, location)) return {};
    const auto path = clipSamplePath(ctx, location);
    const bool found = ! path.empty();
    const auto* song = ctx.song;
    auto valid = [&ctx, song, location] { return ctx.song == song && audioClip(ctx, location) != nullptr; };
    const bool editable = canReplace(ctx, location);
    return {
        {"Show in Browser", [&ctx, valid, location] { if (valid()) showInBrowser(ctx, clipSamplePath(ctx, location)); }, found},
        {"Show in Finder", [&ctx, valid, location] {
            if (valid() && ctx.revealInFinder) if (const auto file = clipSamplePath(ctx, location); ! file.empty()) ctx.revealInFinder(file);
        }, found && static_cast<bool>(ctx.revealInFinder)},
        {"Show Similar Files", [&ctx, valid, location] { if (valid()) showSimilarFiles(ctx, clipSamplePath(ctx, location), location); },
         found && static_cast<bool>(ctx.findSimilarSamples)},
        {"Replace Sample...", [&ctx, valid, location] { if (valid()) beginSampleSwap(ctx, location); }, editable},
    };
}

void addSampleScopeMenu(juce::PopupMenu& menu, UiContext& ctx, model::ClipLocation location)
{
    if (! audioClip(ctx, location)) return;
    juce::PopupMenu scopes;
    for (const auto scope : {model::SampleScope::Clip, model::SampleScope::Track, model::SampleScope::Set})
    {
        const auto count = model::clipsUsingSample(*ctx.song, location, scope).size();
        juce::String label(model::sampleScopeLabel(scope));
        if (scope != model::SampleScope::Clip) label << " (" << (int) count << ")";
        juce::PopupMenu::Item item(label);
        item.isTicked = ctx.sampleReplaceScope == scope;
        item.action = [&ctx, scope] { ctx.sampleReplaceScope = scope; if (ctx.requestRepaint) ctx.requestRepaint(); };
        scopes.addItem(std::move(item));
    }
    menu.addSubMenu("Replace In", scopes);
}

} // namespace sonora::ui
