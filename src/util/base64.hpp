#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::base64 {

// java.util.Base64 (the basic alphabet, with padding).
std::string encode(std::span<const uint8_t> data);
// Accepts padded or unpadded input; empty on invalid characters.
std::optional<std::vector<uint8_t>> decode(std::string_view text);

} // namespace mc::base64
