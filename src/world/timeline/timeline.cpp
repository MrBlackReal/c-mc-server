#include "world/timeline/timeline.hpp"

#include <vector>

#include "generated/behaviour.hpp"

namespace mc::timeline {

std::optional<json::Value> sample(std::string_view name, std::string_view attribute, int64_t ticks) {
    static const json::Value all = json::parse(gen::kTimelinesJson);
    const json::Value& timeline = all[name];
    const json::Value& track = timeline["tracks"][attribute];
    if (!track.isObject()) return std::nullopt;
    const auto& keyframes = track["keyframes"].array();
    if (keyframes.empty()) return std::nullopt;
    if (keyframes.size() == 1) return keyframes[0]["value"];

    // KeyframeTrackSampler.bakeSegments: with a period, wrap-around segments at both ends.
    struct Segment {
        const json::Value* from;
        int64_t fromTicks;
        const json::Value* to;
        int64_t toTicks;
    };
    std::vector<Segment> segments;
    bool periodic = timeline.has("period_ticks");
    int64_t period = static_cast<int64_t>(timeline["period_ticks"].number());
    const json::Value& first = keyframes.front();
    const json::Value& last = keyframes.back();
    auto at = [](const json::Value& k) { return static_cast<int64_t>(k["ticks"].number()); };
    if (periodic) segments.push_back({&last["value"], at(last) - period, &first["value"], at(first)});
    for (size_t i = 0; i + 1 < keyframes.size(); i++)
        segments.push_back({&keyframes[i]["value"], at(keyframes[i]), &keyframes[i + 1]["value"], at(keyframes[i + 1])});
    if (periodic) segments.push_back({&last["value"], at(last), &first["value"], at(first) + period});

    int64_t t = periodic ? ((ticks % period) + period) % period : ticks;
    const Segment* segment = &segments.back();
    for (const auto& s : segments)
        if (t < s.toTicks) {
            segment = &s;
            break;
        }
    if (t <= segment->fromTicks) return *segment->from;
    if (t >= segment->toTicks) return *segment->to;
    if (!segment->from->isNumber() || track["ease"].isString() && track["ease"].string() == "constant") return *segment->from;
    // TODO: easing types other than linear and constant (no vanilla gameplay track uses them).
    float alpha = static_cast<float>(t - segment->fromTicks) / static_cast<float>(segment->toTicks - segment->fromTicks);
    double a = segment->from->number(), b = segment->to->number();
    return json::Value{a + alpha * (b - a)};
}

} // namespace mc::timeline
