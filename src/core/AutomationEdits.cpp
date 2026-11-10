#include "core/AutomationEdits.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace sonora::model
{

namespace
{
constexpr double eps = 1e-9;

void sortPoints(AutomationEnvelope& envelope)
{
    std::stable_sort(envelope.points.begin(), envelope.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
}

AutomationPoint straight(Beats time, double value) { return {time, value}; }

double snapStepped(const AutomationEnvelope& envelope, double value)
{
    return envelope.stepped ? std::round(value) : value;
}

// Drops a point that repeats its neighbours' value at the same time (an edge pin equal to what follows it).
void dropRedundantJumps(AutomationEnvelope& envelope)
{
    auto& p = envelope.points;
    for (size_t i = 1; i < p.size();)
    {
        if (p[i].time >= 0 && std::abs(p[i].time - p[i - 1].time) < eps && std::abs(p[i].value - p[i - 1].value) < 1e-12)
            p.erase(p.begin() + (ptrdiff_t) i);
        else
            ++i;
    }
}
} // namespace

double laneValueAt(const AutomationEnvelope& envelope, Beats beat, const LaneScale& scale)
{
    const auto& points = envelope.points;
    if (points.empty()) return 0.0;
    const auto next = std::upper_bound(points.begin(), points.end(), beat, [](Beats t, const AutomationPoint& p) { return t < p.time; });
    if (next == points.begin()) return next->value;
    const auto& prev = *(next - 1);
    if (next == points.end() || envelope.stepped) return prev.value;
    const double shape = segmentShape(prev, (beat - prev.time) / std::max(eps, next->time - prev.time));
    if (scale.law == AutomationLaw::Linear) return prev.value + (next->value - prev.value) * shape;
    const double a = scale.position(prev.value), b = scale.position(next->value);
    return scale.value(a + (b - a) * shape);
}

EnvelopeClip copyEnvelopeRange(const AutomationEnvelope& envelope, Beats start, Beats end, const LaneScale& scale)
{
    EnvelopeClip clip;
    if (! (end > start)) return clip;
    clip.length = end - start;
    clip.stepped = envelope.stepped;
    clip.scale = scale;
    clip.points.push_back(straight(0.0, laneValueAt(envelope, start, scale)));
    for (const auto& p : envelope.points)
        if (p.time > start + eps && p.time < end - eps)
        {
            auto q = p;
            q.time -= start;
            clip.points.push_back(q);
        }
    clip.points.push_back(straight(clip.length, laneValueAt(envelope, std::nextafter(end, -INFINITY), scale)));
    return clip;
}

void eraseEnvelopeRange(AutomationEnvelope& envelope, Beats start, Beats end, const LaneScale& scale)
{
    if (! (end > start)) return;
    start = std::max(0.0, start);
    const double before = laneValueAt(envelope, std::nextafter(start, -INFINITY), scale);
    const double after = laneValueAt(envelope, end, scale);
    std::erase_if(envelope.points, [&](const AutomationPoint& p) { return p.time >= 0 && p.time >= start - eps && p.time <= end + eps; });
    // The segment that led into the range keeps its shape only up to the new edge point: straighten what now ends there.
    envelope.points.push_back(straight(start, before));
    envelope.points.push_back(straight(end, after));
    sortPoints(envelope);
    for (auto& p : envelope.points)
        if (p.time >= start - eps && p.time < end - eps) p.x1 = p.y1 = p.x2 = p.y2 = 0.5;
    dropRedundantJumps(envelope);
}

void pasteEnvelopeClip(AutomationEnvelope& envelope, Beats at, const EnvelopeClip& clip, const LaneScale& scale)
{
    if (clip.empty()) return;
    at = std::max(0.0, at);
    const double end = at + clip.length;
    const double before = laneValueAt(envelope, std::nextafter(at, -INFINITY), scale);
    const double after = laneValueAt(envelope, end, scale);
    const bool sameScale = clip.scale.law == scale.law && std::abs(clip.scale.min - scale.min) < 1e-12 && std::abs(clip.scale.max - scale.max) < 1e-12;
    std::erase_if(envelope.points, [&](const AutomationPoint& p) { return p.time >= 0 && p.time >= at - eps && p.time <= end + eps; });
    std::vector<AutomationPoint> inserted;
    inserted.push_back(straight(at, before));
    for (const auto& p : clip.points)
    {
        auto q = p;
        q.time = at + p.time;
        if (! sameScale) q.value = scale.value(clip.scale.position(p.value));
        q.value = snapStepped(envelope, q.value);
        inserted.push_back(q);
    }
    inserted.back().x1 = inserted.back().y1 = inserted.back().x2 = inserted.back().y2 = 0.5;
    inserted.push_back(straight(end, after));
    envelope.points.insert(envelope.points.end(), inserted.begin(), inserted.end());
    sortPoints(envelope);
    dropRedundantJumps(envelope);
}

std::vector<Beats> moveEnvelopePoints(AutomationEnvelope& envelope, const std::vector<Beats>& times, double deltaBeats,
                                      double deltaPosition, const LaneScale& scale)
{
    std::vector<bool> moving(envelope.points.size(), false);
    std::set<size_t> used;
    for (const Beats t : times)
        for (size_t i = 0; i < envelope.points.size(); ++i)
            if (! used.count(i) && envelope.points[i].time >= 0 && std::abs(envelope.points[i].time - t) < eps)
            {
                moving[i] = true;
                used.insert(i);
                break;
            }
    if (used.empty()) return {};
    double first = INFINITY, last = -INFINITY;
    for (size_t i : used) { first = std::min(first, envelope.points[i].time); last = std::max(last, envelope.points[i].time); }
    deltaBeats = std::max(deltaBeats, -first); // never before the song start
    const double from = std::min(first, first + deltaBeats), to = std::max(last, last + deltaBeats);
    std::vector<AutomationPoint> kept, moved;
    std::vector<Beats> newTimes;
    for (size_t i = 0; i < envelope.points.size(); ++i)
    {
        auto p = envelope.points[i];
        if (moving[i])
        {
            p.time += deltaBeats;
            p.value = snapStepped(envelope, scale.value(scale.position(p.value) + deltaPosition));
            moved.push_back(p);
            newTimes.push_back(p.time);
        }
        else if (p.time < 0 || std::abs(deltaBeats) < eps || p.time < from - eps || p.time > to + eps
                 || (p.time > first - eps && p.time < last + eps)) // between moved points: a range move keeps them
            kept.push_back(p);
        // else: the moved block passed over it (the reference removes it)
    }
    envelope.points = std::move(kept);
    envelope.points.insert(envelope.points.end(), moved.begin(), moved.end());
    sortPoints(envelope);
    return newTimes;
}

std::pair<int, int> segmentAt(const AutomationEnvelope& envelope, Beats beat)
{
    const auto& p = envelope.points;
    for (size_t i = 0; i + 1 < p.size(); ++i)
        if (p[i].time >= 0 && p[i].time <= beat && beat < p[i + 1].time) return {(int) i, (int) i + 1};
    return {-1, -1};
}

void curveSegment(AutomationEnvelope& envelope, int index, double amount)
{
    if (index < 0 || (size_t) index + 1 >= envelope.points.size() || envelope.stepped) return;
    const double c = std::clamp(amount, -0.95, 0.95);
    auto& p = envelope.points[(size_t) index];
    p.x1 = p.x2 = 0.5 - 0.5 * c;
    p.y1 = p.y2 = 0.5 + 0.5 * c;
    if (std::abs(c) < 1e-3) p.x1 = p.y1 = p.x2 = p.y2 = 0.5;
}

double segmentCurvature(const AutomationPoint& start)
{
    if (! start.curved()) return 0.0;
    return std::clamp(((start.y1 - start.x1) + (start.y2 - start.x2)) * 0.5, -1.0, 1.0);
}

void simplifyEnvelope(AutomationEnvelope& envelope, Beats start, Beats end, const LaneScale& scale, double tolerance)
{
    auto& p = envelope.points;
    if (p.size() < 3) return;
    // Iterative: drop the inner point with the smallest deviation while it is under the tolerance.
    for (;;)
    {
        double best = tolerance;
        int bestIndex = -1;
        for (size_t i = 1; i + 1 < p.size(); ++i)
        {
            const auto &a = p[i - 1], &b = p[i], &c = p[i + 1];
            if (b.time < 0 || b.time < start - eps || b.time > end + eps || a.time < 0) continue;
            if (a.curved() || b.curved()) continue;
            if (std::abs(b.time - a.time) < eps || std::abs(c.time - b.time) < eps) continue; // jumps stay
            const double pa = scale.position(a.value), pb = scale.position(b.value), pc = scale.position(c.value);
            const double line = envelope.stepped ? pa : pa + (pc - pa) * (b.time - a.time) / (c.time - a.time);
            if (envelope.stepped && std::abs(pb - pa) > 1e-12) continue;
            const double deviation = std::abs(pb - line);
            if (deviation <= best) { best = deviation; bestIndex = (int) i; }
        }
        if (bestIndex < 0) break;
        p.erase(p.begin() + bestIndex);
    }
}

const char* envelopeShapeName(EnvelopeShape shape)
{
    switch (shape)
    {
        case EnvelopeShape::Sine: return "Sine";
        case EnvelopeShape::Triangle: return "Triangle";
        case EnvelopeShape::Sawtooth: return "Sawtooth";
        case EnvelopeShape::InverseSawtooth: return "Inverse Sawtooth";
        case EnvelopeShape::Square: return "Square";
        case EnvelopeShape::RampUp: return "Ramp Up";
        case EnvelopeShape::RampDown: return "Ramp Down";
        case EnvelopeShape::Adsr: return "ADSR";
    }
    return "";
}

void insertEnvelopeShape(AutomationEnvelope& envelope, Beats start, Beats end, EnvelopeShape shape, int cycles, double low,
                         double high, const LaneScale& scale)
{
    if (! (end > start)) return;
    start = std::max(0.0, start);
    cycles = std::clamp(cycles, 1, 256);
    low = std::clamp(low, 0.0, 1.0);
    high = std::clamp(high, 0.0, 1.0);
    const double length = end - start, before = scale.position(laneValueAt(envelope, std::nextafter(start, -INFINITY), scale));
    std::vector<std::pair<double, double>> shapePoints; // (fraction of the range, lane position)
    const double period = 1.0 / cycles;
    switch (shape)
    {
        case EnvelopeShape::Sine:
            for (int c = 0; c < cycles; ++c)
                for (int k = 0; k < 16; ++k)
                {
                    const double f = (c + k / 16.0) * period;
                    shapePoints.push_back({f, low + (high - low) * (0.5 + 0.5 * std::sin(2 * M_PI * k / 16.0))});
                }
            shapePoints.push_back({1.0, low + (high - low) * 0.5});
            break;
        case EnvelopeShape::Triangle:
            for (int c = 0; c < cycles; ++c)
            {
                shapePoints.push_back({c * period, low});
                shapePoints.push_back({(c + 0.5) * period, high});
            }
            shapePoints.push_back({1.0, low});
            break;
        case EnvelopeShape::Sawtooth:
        case EnvelopeShape::InverseSawtooth:
        {
            const bool up = shape == EnvelopeShape::Sawtooth;
            for (int c = 0; c < cycles; ++c)
            {
                shapePoints.push_back({c * period, up ? low : high});
                shapePoints.push_back({(c + 1) * period, up ? high : low});
            }
            break;
        }
        case EnvelopeShape::Square:
            for (int c = 0; c < cycles; ++c)
            {
                shapePoints.push_back({c * period, high});
                shapePoints.push_back({(c + 0.5) * period, high});
                shapePoints.push_back({(c + 0.5) * period, low});
                shapePoints.push_back({(c + 1) * period, low});
            }
            break;
        case EnvelopeShape::RampUp:
            shapePoints = {{0.0, before}, {1.0, high}};
            break;
        case EnvelopeShape::RampDown:
            shapePoints = {{0.0, before}, {1.0, low}};
            break;
        case EnvelopeShape::Adsr:
            // Attack to the top, decay to the middle of the swing, sustain, release back to the value before.
            shapePoints = {{0.0, before}, {0.1, high}, {0.3, low + (high - low) * 0.6}, {0.75, low + (high - low) * 0.6}, {1.0, before}};
            break;
    }
    EnvelopeClip clip;
    clip.length = length;
    clip.scale = scale;
    clip.stepped = envelope.stepped;
    for (const auto& [f, pos] : shapePoints)
    {
        AutomationPoint p = straight(f * length, scale.value(pos));
        // Adjacent equal times (the square's edges) stay in order: stable sort in paste.
        clip.points.push_back(p);
    }
    pasteEnvelopeClip(envelope, start, clip, scale);
}

std::vector<Beats> envelopePointTimes(const AutomationEnvelope& envelope, Beats start, Beats end)
{
    std::vector<Beats> out;
    for (const auto& p : envelope.points)
        if (p.time >= 0 && p.time >= start - eps && p.time <= end + eps) out.push_back(p.time);
    return out;
}

bool envelopeMovesIn(const AutomationEnvelope& envelope, Beats start, Beats end)
{
    if (! (end > start)) return false;
    bool any = false;
    double lo = INFINITY, hi = -INFINITY;
    for (const auto& p : envelope.points)
    {
        if (p.time < 0) continue;
        any = true;
        if (p.time >= start - eps && p.time < end - eps) return true;
        lo = std::min(lo, p.time); hi = std::max(hi, p.time);
    }
    // No breakpoint inside, but the range lies on a ramp between two of them.
    if (! any || envelope.stepped) return false;
    const LaneScale linear;
    return lo < start && hi > end && std::abs(laneValueAt(envelope, start, linear) - laneValueAt(envelope, end, linear)) > 1e-9;
}

std::string formatLaneValue(double value, const LaneScale& scale, bool pan, bool toggle, const std::string& unit)
{
    char buf[48];
    if (toggle) return value >= 0.5 ? "On" : "Off";
    if (pan)
    {
        const int amount = (int) std::lround(std::abs(value) * 50.0);
        if (amount == 0) return "C";
        std::snprintf(buf, sizeof(buf), "%d%c", amount, value < 0 ? 'L' : 'R');
        return buf;
    }
    if (scale.law == AutomationLaw::Fader)
    {
        if (value <= 1e-6) return "-inf dB";
        std::snprintf(buf, sizeof(buf), "%.1f dB", 20.0 * std::log10(value));
        return buf;
    }
    if (unit == "Hz" && value >= 1000.0) std::snprintf(buf, sizeof(buf), "%.2f kHz", value / 1000.0);
    else std::snprintf(buf, sizeof(buf), unit.empty() ? "%.3g" : "%.3g %s", value, unit.c_str());
    return buf;
}

bool parseLaneValue(const std::string& text, const LaneScale& scale, bool pan, bool toggle, double& value)
{
    std::string s;
    for (char ch : text) if (ch != ' ') s.push_back((char) std::tolower((unsigned char) ch));
    if (s.empty()) return false;
    if (toggle)
    {
        if (s == "on" || s == "1") { value = 1; return true; }
        if (s == "off" || s == "0") { value = 0; return true; }
        return false;
    }
    if (pan)
    {
        if (s == "c" || s == "center" || s == "centre") { value = 0; return true; }
        const char side = s.back();
        if (side == 'l' || side == 'r')
        {
            char* end = nullptr;
            const double amount = std::strtod(s.c_str(), &end);
            if (end == s.c_str()) return false;
            value = std::clamp(amount / 50.0, 0.0, 1.0) * (side == 'l' ? -1.0 : 1.0);
            return true;
        }
    }
    if (scale.law == AutomationLaw::Fader)
    {
        if (s.rfind("-inf", 0) == 0) { value = 0; return true; }
        char* end = nullptr;
        const double db = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || ! std::isfinite(db)) return false;
        value = std::clamp(std::pow(10.0, db / 20.0), scale.min, scale.max > scale.min ? scale.max : 1.995262315);
        return true;
    }
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || ! std::isfinite(v)) return false;
    const std::string rest(end);
    if (! rest.empty() && rest[0] == 'k') v *= 1000.0;
    if (pan) v = std::clamp(v / 50.0, -1.0, 1.0);
    value = std::clamp(v, std::min(scale.min, scale.max), std::max(scale.min, scale.max));
    return true;
}

} // namespace sonora::model
