#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "util/json.hpp"

namespace mc::timeline {

// Timeline + KeyframeTrackSampler: the value of `attribute`'s track in the timeline `name`
// (e.g. "minecraft:day") at `ticks` on its clock, before the track's modifier is applied.
// Numbers are interpolated (unless the track eases "constant"); other values step at keyframes.
// Empty if the timeline has no such track.
std::optional<json::Value> sample(std::string_view name, std::string_view attribute, int64_t ticks);

} // namespace mc::timeline
