#pragma once

#include "geoworld/spatial/coordinates.hpp"

#include <cstdint>
#include <optional>

namespace geoworld::spatial {

struct S2GlobalKey {
    std::uint64_t value{};
    std::uint8_t level{};

    bool operator==(const S2GlobalKey&) const = default;
};

[[nodiscard]] std::optional<S2GlobalKey> s2_key_for(Geodetic coordinate,
                                                     std::uint8_t level);

} // namespace geoworld::spatial
