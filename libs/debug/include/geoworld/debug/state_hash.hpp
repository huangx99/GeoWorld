#pragma once

#include "geoworld/world/world.hpp"

#include <cstdint>

namespace geoworld::debug {

[[nodiscard]] std::uint64_t world_state_hash(const world::World& world);

} // namespace geoworld::debug
