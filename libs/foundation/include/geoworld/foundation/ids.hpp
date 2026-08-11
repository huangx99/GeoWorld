#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace geoworld::foundation {

struct WorldId {
    std::uint64_t value{};

    constexpr auto operator<=>(const WorldId&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
};

struct RuntimeId {
    std::uint32_t slot{};
    std::uint32_t generation{};

    constexpr auto operator<=>(const RuntimeId&) const = default;
    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
};

struct FeatureId {
    std::uint32_t value{};

    constexpr auto operator<=>(const FeatureId&) const = default;
};

struct WorldIdHash {
    std::size_t operator()(WorldId id) const noexcept {
        return static_cast<std::size_t>(id.value ^ (id.value >> 32U));
    }
};

} // namespace geoworld::foundation
