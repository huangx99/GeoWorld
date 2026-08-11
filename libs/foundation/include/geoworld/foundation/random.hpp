#pragma once

#include <cstdint>

namespace geoworld::foundation {

class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next_u64() noexcept;
    [[nodiscard]] double next_unit() noexcept;

private:
    std::uint64_t state_;
};

} // namespace geoworld::foundation
