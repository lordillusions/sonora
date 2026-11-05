#include "ui/views/CollabPanel.h"

#include "gfx/Fonts.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

namespace sonora::ui
{

namespace
{
constexpr float kPanelW = 600.0f, kPanelH = 560.0f, kRowH = 26.0f;

std::string initials(const std::string& name)
{
    std::string out;
    bool take = true;
    for (size_t i = 0; i < name.size() && out.size() < 2; ++i)
    {
        const auto ch = static_cast<unsigned char>(name[i]);
        if (ch == ' ' || ch == '-' || ch == '_') { take = true; continue; }
        if (take && ch < 0x80) out.push_back(static_cast<char>(std::toupper(ch)));
        take = false;
    }
    return out.empty() ? std::string("?") : out;
}

constexpr SkColor kTalking = SkColorSetRGB(39, 174, 96);

void avatar(SkCanvas& c, float cx, float cy, float radius, int color, const std::string& name, SkColor ring, bool talking = false)
{
    SkPaint p;
    p.setAntiAlias(true);
    if (talking) // COL-7: a green ring while their voice is heard
    {
        p.setColor(kTalking);
        c.drawCircle(cx, cy, radius + 3.5f, p);
    }
    p.setColor(ring);
    c.drawCircle(cx, cy, radius + 1.5f, p);
    p.setColor(collabColor(color));
    c.drawCircle(cx, cy, radius, p);
    paint::text(c, initials(name), {cx - radius, cy - radius, radius * 2, radius * 2}, SK_ColorWHITE, radius * 0.95f, Align::Centre, true);
}

// COL-7: a small microphone after a name: drawn when the mic is on (green while talking, struck through when muted here).
void micIcon(SkCanvas& c, float x, float cy, SkColor color, bool muted)
{
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x + 2, cy - 7, 6, 9), 3, 3), p);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(1.2f);
    c.drawArc(SkRect::MakeXYWH(x, cy - 4, 10, 8), 0, 180, false, p);
    c.drawLine(x + 5, cy + 4, x + 5, cy + 6.5f, p);
    if (muted) c.drawLine(x - 1, cy - 7, x + 11, cy + 6, p);
}

// COL-8: a small camera after a name while the camera is on.
void cameraIcon(SkCanvas& c, float x, float cy, SkColor color)
{
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(x, cy - 4, 9, 8), 2, 2), p);
    SkPath lens;
    lens.moveTo(x + 9, cy);
    lens.lineTo(x + 13, cy - 3.5f);
    lens.lineTo(x + 13, cy + 3.5f);
    lens.close();
    c.drawPath(lens, p);
}

const char* roleText(CollabView::Role r)
{
    switch (r)
    {
        case CollabView::Role::Host: return "Host";
        case CollabView::Role::Editor: return "Can edit";
        case CollabView::Role::Listener: return "Listens";
    }
    return "";
}
} // namespace

Rect CollabPanel::panelRect() const
{
    // Before a session the panel only offers Share and Join: shorter, unless nearby sessions need rows.
    const auto& v = ctx().collab;
    const float wanted = v.active() || ! v.networkAvailable ? kPanelH : 367.0f + 26.0f * (float) std::min<size_t>(v.lanSessions.size(), 5)
                         + (v.lanSessions.empty() ? 0.0f : 30.0f) + (v.endedReason.empty() ? 0.0f : 36.0f);
    const float w = std::min(kPanelW, width() - 40), h = std::min(wanted, height() - 40);
    return {(width() - w) * 0.5f, (height() - h) * 0.5f, w, h};
}

void CollabPanel::button(SkCanvas& c, const Theme& t, const Rect& r, const std::string& label, std::function<void()> action, bool enabled, bool primary)
{
    const auto bg = primary && enabled ? t.color("ChosenDefault") : t.color("ControlBackground");
    paint::fill(c, r, bg);
    paint::stroke(c, r, t.color("ControlContrastFrame"));
    paint::text(c, label, r.reduced(4, 0), enabled ? (primary ? paint::contrastText(bg) : t.color("ControlForeground")) : t.color("TextDisabled"),
                t.metrics.fontSize, Align::Centre);
    if (enabled) hits_.push_back({r, std::move(action)});
}

void CollabPanel::toggle(SkCanvas& c, const Theme& t, const Rect& area, const std::string& label, bool on, bool enabled,
                         std::function<void()> action)
{
    // A small switch and its label; disabled ones (not built yet, or set by the host) are drawn dim.
    const Rect track{area.x, area.centreY() - 7, 26, 14};
    SkPaint p;
    p.setAntiAlias(true);
    const SkColor fill = on ? t.color("ChosenDefault") : t.color("ControlBackground");
    p.setColor(enabled ? fill : paint::withAlpha(fill, 110));
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(track.x, track.y, track.w, track.h), 7, 7), p);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(1.0f);
    p.setColor(t.color("ControlContrastFrame"));
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(track.x, track.y, track.w, track.h), 7, 7), p);
    p.setStyle(SkPaint::kFill_Style);
    p.setColor(enabled ? SK_ColorWHITE : paint::withAlpha(SK_ColorWHITE, 150));
    c.drawCircle(on ? track.right() - 7 : track.x + 7, track.centreY(), 5.0f, p);
    paint::text(c, label, {area.x + 32, area.y, area.w - 32, area.h}, enabled ? t.color("ControlForeground") : t.color("TextDisabled"),
                t.metrics.fontSize, Align::Left);
    if (enabled) hits_.push_back({area, std::move(action)});
}

void CollabPanel::policyRow(SkCanvas& c, const Theme& t, Rect line, bool editable)
{
    // Chat, voice and camera for the session (COL-9). Voice and camera are drawn off until COL-7 / COL-8 build them.
    auto& x = ctx();
    const auto& v = x.collab;
    auto set = [&x](CollabView::Policy p) { if (x.collabSetPolicy) x.collabSetPolicy(p); };
    auto policy = v.policy;
    toggle(c, t, line.removeFromLeft(100), "Chat", policy.chat, editable, [set, policy]() mutable { policy.chat = ! policy.chat; set(policy); });
    toggle(c, t, line.removeFromLeft(150), v.voiceSupported ? std::string("Voice") : std::string("Voice (COL-7)"), policy.voice && v.voiceSupported,
           editable && v.voiceSupported, [set, policy]() mutable { policy.voice = ! policy.voice; set(policy); });
    toggle(c, t, line.removeFromLeft(160), v.videoSupported ? std::string("Camera") : std::string("Camera (COL-8)"), policy.video && v.videoSupported,
           editable && v.videoSupported, [set, policy]() mutable { policy.video = ! policy.video; set(policy); });
}

void CollabPanel::close()
{
    auto& x = ctx();
    x.collabOpen = false;
    if (x.modelChanged) x.modelChanged();
}

void CollabPanel::paint(SkCanvas& c, const Theme& t)
{
    hits_.clear();
    auto& x = ctx();
    const auto& v = x.collab;
    paint::fill(c, localBounds(), paint::withAlpha(SkColorSetRGB(0, 0, 0), 150)); // scrim
    const auto panel = panelRect();
    paint::fill(c, panel, t.color("SurfaceBackground"));
    paint::stroke(c, panel, t.color("ControlContrastFrame"));
    const SkColor ink = t.color("ControlForeground"), dim = t.color("TextDisabled"), alert = t.color("ChosenAlert");
    const float fs = t.metrics.fontSize;

    auto r = panel;
    auto title = r.removeFromTop(32);
    paint::text(c, "Collaborate", title.reduced(14, 0), ink, fs + 3, Align::Left, true);
    button(c, t, {panel.right() - 74, panel.y + 6, 62, 20}, "Close", [this] { close(); });
    r = r.reduced(16, 2);
    auto heading = [&](const std::string& text) {
        r.removeFromTop(6);
        paint::text(c, text, r.removeFromTop(20), ink, fs + 1, Align::Left, true);
    };
    auto note = [&](const std::string& text, SkColor color) { paint::text(c, text, r.removeFromTop(18), color, fs - 1, Align::Left); };
    auto call = [](const std::function<void()>& f) { if (f) f(); };

    if (! v.networkAvailable)
    {
        note("This build of Sonora was made without collab networking (SONORA_COLLAB_NET).", alert);
        note("Run scripts/bootstrap.sh and rebuild to enable sessions.", dim);
        return;
    }

    const bool active = v.active();
    if (! active)
    {
        // Before a session: one way to share (internet by default, Wi-Fi as the alternative) and one way to join.
        {
            auto line = r.removeFromTop(kRowH);
            paint::text(c, "Your name", line.removeFromLeft(110), dim, fs, Align::Left);
            const auto name = line.removeFromLeft(220).reduced(0, 2);
            button(c, t, name, v.displayName.empty() ? std::string("Set your name...") : v.displayName, [&x, call] { call(x.collabEditName); });
        }

        heading("Invite people to this set");
        {
            // A radio choice, not two more buttons: Start Sharing stays the one action.
            auto line = r.removeFromTop(kRowH).reduced(0, 2);
            paint::text(c, "They are", line.removeFromLeft(110), dim, fs, Align::Left);
            auto radio = [&](Rect area, const std::string& label, bool chosen, bool lan) {
                SkPaint p;
                p.setAntiAlias(true);
                const float cx = area.x + 7, cy = area.centreY();
                p.setStyle(SkPaint::kStroke_Style);
                p.setStrokeWidth(1.2f);
                p.setColor(chosen ? t.color("ChosenDefault") : t.color("ControlContrastFrame"));
                c.drawCircle(cx, cy, 6.0f, p);
                if (chosen)
                {
                    p.setStyle(SkPaint::kFill_Style);
                    c.drawCircle(cx, cy, 3.5f, p);
                }
                paint::text(c, label, {area.x + 20, area.y, area.w - 20, area.h}, ink, fs, Align::Left, chosen);
                hits_.push_back({area, [&x, lan] { x.collab.shareOnLan = lan; }});
            };
            radio(line.removeFromLeft(190), "Anywhere (internet)", ! v.shareOnLan, false);
            radio(line.removeFromLeft(150), "On my Wi-Fi", v.shareOnLan, true);
        }
        r.removeFromTop(4);
        {
            auto line = r.removeFromTop(kRowH + 6).reduced(0, 2);
            line.removeFromLeft(110);
            button(c, t, line.removeFromLeft(220), "Start Sharing", [&x, lan = v.shareOnLan] { if (x.collabShare) x.collabShare(lan); }, x.song != nullptr, true);
        }
        r.removeFromTop(2);
        auto indented = [&](const std::string& text, SkColor color) {
            auto line = r.removeFromTop(18);
            line.removeFromLeft(110);
            paint::text(c, text, line, color, fs - 1, Align::Left);
        };
        if (x.song == nullptr) indented("Open or create a set first.", alert);
        else if (v.shareOnLan)
        {
            indented("No internet needed. People on the same Wi-Fi or studio network", dim);
            indented("pick your set from their Join list, or open your invite link.", dim);
        }
        else
        {
            indented("You get an invite link (already copied) to send by chat or email.", dim);
            indented("The music goes directly between you, encrypted.", dim);
        }
        {
            r.removeFromTop(4);
            auto line = r.removeFromTop(kRowH).reduced(0, 2);
            paint::text(c, "Allow", line.removeFromLeft(110), dim, fs, Align::Left);
            policyRow(c, t, line, true);
        }
        if (! v.endedReason.empty())
        {
            // Word-wrapped: a failed share's reason says what to do and must be read whole.
            const float maxW = r.w - 110;
            std::string line, word;
            const std::string text = "Last session: " + v.endedReason + " ";
            for (char ch : text)
            {
                if (ch != ' ') { word.push_back(ch); continue; }
                const auto trial = line.empty() ? word : line + " " + word;
                if (! line.empty() && gfx::measureText(trial, fs - 1) > maxW) { indented(line, alert); line = word; }
                else line = trial;
                word.clear();
            }
            if (! line.empty()) indented(line, alert);
        }

        heading("Join someone's session");
        {
            auto line = r.removeFromTop(kRowH + 6).reduced(0, 2);
            line.removeFromLeft(110);
            button(c, t, line.removeFromLeft(220), "Join with Invite Link...", [&x, call] { call(x.collabJoinLink); }, true, true);
        }
        r.removeFromTop(2);
        indented("Paste the link you were sent. No account needed.", dim);
        if (! v.lanSessions.empty())
        {
            r.removeFromTop(6);
            paint::text(c, "On your Wi-Fi", r.removeFromTop(20), ink, fs, Align::Left, true);
            for (const auto& s : v.lanSessions)
            {
                if (r.h < 56) break;
                auto line = r.removeFromTop(kRowH).reduced(0, 2);
                const auto ask = line.removeFromRight(110);
                paint::text(c, s.label, line, ink, fs, Align::Left);
                button(c, t, ask, "Ask to Join", [&x, address = s.address] { if (x.collabJoinNearby) x.collabJoinNearby(address); });
            }
        }

        // Advanced, at the bottom: which server introduces you over the internet, and an optional access key.
        auto bottom = panel.reduced(16, 0).removeFromBottom(34).reduced(0, 7);
        const auto settingsButton = bottom.removeFromRight(170);
        std::string server = v.relayUrl;
        for (const char* scheme : {"https://", "http://"})
            if (server.rfind(scheme, 0) == 0) server = server.substr(std::string(scheme).size());
        paint::text(c, v.relayIsDefault ? "Server: " + server + " (Sonora)" : "Server: " + server, bottom, dim, fs - 1, Align::Left);
        button(c, t, settingsButton, "Connection Settings...", [&x, call] { call(x.collabEditRelay); });
        return;
    }

    // ---- in a session
    {
        auto line = r.removeFromTop(22);
        const std::string who = v.host ? "You are hosting" : "You joined";
        paint::text(c, who + (v.sessionName.empty() ? std::string() : ": " + v.sessionName), line, ink, fs + 1, Align::Left, true);
    }
    const bool connecting = v.state != CollabView::State::Active;
    if (! v.status.empty() || connecting)
    {
        note(v.status.empty() ? std::string("Connecting...") : v.status, v.state == CollabView::State::Reconnecting ? alert : dim);
        if (v.progress >= 0.0f)
        {
            const auto bar = r.removeFromTop(10).reduced(0, 2);
            paint::fill(c, bar, t.color("RetroDisplayBackground"));
            paint::fill(c, {bar.x, bar.y, bar.w * std::clamp(v.progress, 0.0f, 1.0f), bar.h}, t.color("ChosenDefault"));
        }
    }
    if (v.host && ! v.invite.empty())
    {
        heading("Invite");
        auto line = r.removeFromTop(kRowH).reduced(0, 2);
        const auto copy = line.removeFromRight(150);
        const auto field = line.withWidth(line.w - 8);
        paint::fill(c, field, t.color("ControlBackground"));
        paint::stroke(c, field, t.color("ControlContrastFrame"));
        paint::text(c, v.invite, field.reduced(6, 0), ink, fs - 1, Align::Left);
        button(c, t, copy, "Copy Invite Link", [&x, call] { call(x.collabCopyInvite); }, true, true);
        note("Anyone with the link can join until you end the session. Send it privately.", dim);
    }

    heading("People");
    for (const auto& p : v.participants)
    {
        if (r.h < 60) break;
        auto line = r.removeFromTop(kRowH + 6);
        avatar(c, line.x + 11, line.centreY(), 10, p.color, p.name, t.color("SurfaceBackground"), p.talking);
        line.removeFromLeft(28);
        auto nameBox = line.removeFromLeft(170);
        const auto label = p.name + (p.self ? " (you)" : std::string());
        paint::text(c, label, nameBox, ink, fs, Align::Left, true);
        float iconX = std::min(nameBox.x + gfx::measureText(label, fs, true) + 6, nameBox.right() - 12);
        if (p.micOn)
        {
            micIcon(c, iconX, nameBox.centreY(), p.talking ? kTalking : dim, p.mutedHere);
            iconX += 14;
        }
        if (p.cameraOn && iconX < nameBox.right() - 12) cameraIcon(c, iconX, nameBox.centreY(), dim);
        auto roleBox = line.removeFromLeft(90).reduced(0, 5);
        if (v.host && ! p.self && p.role != CollabView::Role::Host)
        {
            const auto next = p.role == CollabView::Role::Listener ? CollabView::Role::Editor : CollabView::Role::Listener;
            button(c, t, roleBox, roleText(p.role), [&x, id = p.id, next] { if (x.collabSetRole) x.collabSetRole(id, next); });
        }
        else paint::text(c, roleText(p.role), roleBox, dim, fs, Align::Left);
        line.removeFromLeft(8);
        if (v.host && ! p.self)
        {
            const auto remove = line.removeFromRight(70).reduced(0, 5);
            button(c, t, remove, "Remove", [&x, id = p.id] { if (x.collabRemove) x.collabRemove(id); });
            line.removeFromRight(6);
        }
        if (! p.self && p.micOn && v.voiceAvailable)
        {
            // Mute is local: this machine stops playing them; nobody else is affected.
            const auto mute = line.removeFromRight(70).reduced(0, 5);
            button(c, t, mute, p.mutedHere ? "Unmute" : "Mute", [&x, id = p.id, muted = p.mutedHere] {
                if (x.collabMuteVoice) x.collabMuteVoice(id, ! muted);
            });
            line.removeFromRight(6);
        }
        std::string detail = p.safetyCode;
        if (! p.self && ! p.hasChat) detail += std::string(detail.empty() ? "" : "  ") + "(older: no chat)";
        if (! detail.empty()) paint::text(c, detail, line, dim, fs - 1, Align::Left);
    }
    if (std::any_of(v.participants.begin(), v.participants.end(), [](const auto& p) { return ! p.safetyCode.empty(); }))
        note("Compare the four words with each person once, by voice: when they match, nobody sits in between.", dim);

    {
        // What the session allows: the host switches it for everyone, guests see it.
        r.removeFromTop(4);
        auto line = r.removeFromTop(kRowH).reduced(0, 2);
        paint::text(c, v.host ? "Allow" : "Host allows", line.removeFromLeft(110), dim, fs, Align::Left);
        policyRow(c, t, line, v.host);
    }
    if (v.voiceSupported)
    {
        // COL-7: this person's mic, push to talk and ducking.
        auto line = r.removeFromTop(kRowH).reduced(0, 2);
        paint::text(c, "Your mic", line.removeFromLeft(110), dim, fs, Align::Left);
        if (! v.voiceAvailable && ! v.micOn) paint::text(c, v.voiceUnavailable, line, dim, fs - 1, Align::Left);
        else
        {
            button(c, t, line.removeFromLeft(96), v.micOn ? "Mic On" : "Mic Off", [&x, on = v.micOn] { if (x.collabSetMic) x.collabSetMic(! on); },
                   v.voiceAvailable || v.micOn, v.micOn);
            line.removeFromLeft(8);
            const auto meter = line.removeFromLeft(56).reduced(0, 8);
            paint::fill(c, meter, t.color("RetroDisplayBackground"));
            const float level = v.micOn ? std::clamp(std::sqrt(v.micLevel) * 1.4f, 0.0f, 1.0f) : 0.0f;
            const bool sending = v.micOn && (! v.pushToTalk || v.pushToTalkHeld);
            paint::fill(c, {meter.x, meter.y, meter.w * level, meter.h}, sending ? kTalking : t.color("TextDisabled"));
            line.removeFromLeft(12);
            toggle(c, t, line.removeFromLeft(150), "Push to talk (`)", v.pushToTalk, true,
                   [&x, on = v.pushToTalk] { if (x.collabSetVoiceOption) x.collabSetVoiceOption(0, ! on); });
            toggle(c, t, line.removeFromLeft(120), "Duck music", v.duck, true,
                   [&x, on = v.duck] { if (x.collabSetVoiceOption) x.collabSetVoiceOption(1, ! on); });
        }
        if (v.micOn && ! v.voiceAvailable) note(v.voiceUnavailable, alert);
        else if (v.micOn && v.noMicInput) note("No audio input is on: choose your microphone in Settings > Audio.", alert);
        else if (v.micOn && v.pushToTalk && ! v.pushToTalkHeld) note("Hold the ` key (left of 1) to talk. Use headphones: no echo cancellation yet.", dim);
        else if (v.micOn) note("Use headphones: Sonora has no echo cancellation yet (COL-9).", dim);
    }
    {
        // COL-8: this person's camera and whether the tiles show here.
        auto line = r.removeFromTop(kRowH).reduced(0, 2);
        paint::text(c, "Your camera", line.removeFromLeft(110), dim, fs, Align::Left);
        if (! v.videoSupported) paint::text(c, "Camera: macOS only for now (COL-8)", line, dim, fs - 1, Align::Left);
        else if (! v.videoAvailable && ! v.cameraOn) paint::text(c, v.videoUnavailable, line, dim, fs - 1, Align::Left);
        else
        {
            button(c, t, line.removeFromLeft(96), v.cameraOn ? "Camera On" : "Camera Off",
                   [&x, on = v.cameraOn] { if (x.collabSetCamera) x.collabSetCamera(! on); }, v.videoAvailable || v.cameraOn, v.cameraOn);
            line.removeFromLeft(76);
            toggle(c, t, line.removeFromLeft(150), "Show video", v.showVideo, true,
                   [&x, on = v.showVideo] { if (x.collabShowVideo) x.collabShowVideo(! on); });
        }
        if (v.cameraOn && ! v.videoAvailable) note(v.videoUnavailable, alert);
    }

    for (const auto& [peer, name] : v.joinRequests)
    {
        auto line = r.removeFromTop(kRowH + 2).reduced(0, 2);
        const auto deny = line.removeFromRight(70);
        line.removeFromRight(6);
        const auto allow = line.removeFromRight(70);
        paint::text(c, name + " asks to join", line, ink, fs, Align::Left, true);
        button(c, t, allow, "Allow", [&x, id = peer] { if (x.collabAnswerJoin) x.collabAnswerJoin(id, true); }, true, true);
        button(c, t, deny, "Deny", [&x, id = peer] { if (x.collabAnswerJoin) x.collabAnswerJoin(id, false); });
    }

    if (! v.host)
    {
        r.removeFromTop(4);
        auto line = r.removeFromTop(kRowH).reduced(0, 2);
        button(c, t, line.removeFromLeft(200), v.followTransport ? "Shared transport: on" : "Shared transport: off",
               [&x, follow = v.followTransport] { if (x.collabSetFollowTransport) x.collabSetFollowTransport(! follow); }, true, v.followTransport);
        line.removeFromLeft(10);
        paint::text(c, v.followTransport ? "Play and Stop start everyone together" : "Play alone to audition; nobody else moves", line, dim, fs - 1, Align::Left);
        if (! v.canEdit && v.state == CollabView::State::Active) note("The host made you a listener: you can follow along but not edit.", dim);
    }

    // Recent notices, newest last, above the buttons.
    auto bottom = panel.reduced(16, 0);
    auto buttons = bottom.removeFromBottom(40).reduced(0, 8);
    const int shown = std::min<int>((int) v.notices.size(), std::max(0, (int) ((r.h - 4) / 16)));
    float y = buttons.y - 4 - 16.0f * (float) shown;
    for (int i = (int) v.notices.size() - shown; i < (int) v.notices.size(); ++i, y += 16)
        paint::text(c, v.notices[(size_t) i], {bottom.x, y, bottom.w, 16}, dim, fs - 1, Align::Left);
    button(c, t, buttons.removeFromLeft(140), v.host ? "End Session" : "Leave Session", [&x, call] { call(x.collabLeave); });
    buttons.removeFromLeft(10);
    std::string chatLabel = x.collabChatOpen ? "Hide Chat" : "Chat";
    if (v.unread > 0 && ! x.collabChatOpen) chatLabel += " (" + std::to_string(v.unread) + " new)";
    button(c, t, buttons.removeFromLeft(140), chatLabel, [&x, call] { call(x.collabToggleChat); }, v.state != CollabView::State::Ended,
           v.unread > 0 && ! x.collabChatOpen);
}

void CollabPanel::mouseDown(const MouseEvent& e)
{
    for (const auto& h : hits_)
        if (h.area.contains(e.x, e.y))
        {
            auto action = h.action; // may rebuild hits_
            action();
            repaint();
            return;
        }
    if (! panelRect().contains(e.x, e.y)) close(); // a click on the scrim closes
    repaint();
}

bool CollabPanel::keyPressed(const KeyEvent& k)
{
    if (! ctx().collabOpen) return false;
    if (k.keyCode == juce::KeyPress::escapeKey) close();
    return true; // modal: nothing underneath reacts to keys
}

float CollabBadge::preferredWidth() const
{
    const auto& v = const_cast<CollabBadge*>(this)->ctx().collab;
    if (! v.active()) return 52.0f;
    return 14.0f + 14.0f * (float) std::max<size_t>(1, std::min<size_t>(v.participants.size(), 4)) + 8.0f + (v.unread > 0 ? 8.0f : 0.0f);
}

void CollabBadge::paint(SkCanvas& c, const Theme& t)
{
    const auto& v = ctx().collab;
    const auto r = localBounds();
    if (! v.active())
    {
        paint::fill(c, r, t.color("ControlBackground"));
        paint::stroke(c, r, t.color("ControlContrastFrame"));
        paint::text(c, "Share", r, t.color("ControlForeground"), t.metrics.fontSize - 1, Align::Centre);
        return;
    }
    // Status dot: green connected, amber connecting or reconnecting.
    SkPaint dot;
    dot.setAntiAlias(true);
    dot.setColor(v.state == CollabView::State::Active ? SkColorSetRGB(39, 174, 96) : SkColorSetRGB(242, 153, 74));
    c.drawCircle(r.x + 5, r.centreY(), 3.5f, dot);
    float cx = r.x + 18;
    size_t drawn = 0;
    for (const auto& p : v.participants)
    {
        if (drawn++ == 4) break;
        avatar(c, cx, r.centreY(), 7.5f, p.color, p.name, t.color("SurfaceBackground"), p.talking);
        cx += 14;
    }
    if (v.unread > 0)
    {
        // Unread chat lines (COL-9): a count over the last avatar.
        const auto count = v.unread > 9 ? std::string("9+") : std::to_string(v.unread);
        const float w = count.size() > 1 ? 15.0f : 11.0f;
        const Rect bubble{r.right() - w, r.y, w, 11};
        SkPaint b;
        b.setAntiAlias(true);
        b.setColor(SkColorSetRGB(235, 87, 87));
        c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(bubble.x, bubble.y, bubble.w, bubble.h), 5.5f, 5.5f), b);
        paint::text(c, count, bubble, SK_ColorWHITE, 8.5f, Align::Centre, true);
    }
}

void CollabBadge::mouseDown(const MouseEvent&)
{
    auto& x = ctx();
    x.collabOpen = ! x.collabOpen;
    if (x.modelChanged) x.modelChanged();
}

} // namespace sonora::ui
