#pragma once

#include "geoworld/ecs/runtime.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace geoworld::ecs {

[[nodiscard]] std::vector<std::uint8_t> encode_snapshot(const RuntimeSnapshot& snapshot);
[[nodiscard]] std::optional<RuntimeSnapshot> decode_snapshot(
    std::span<const std::uint8_t> buffer);

} // namespace geoworld::ecs
