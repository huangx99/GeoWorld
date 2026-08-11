#include "geoworld/foundation/random.hpp"

namespace geoworld::foundation {

std::uint64_t DeterministicRng::next_u64() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    auto value = state_;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

double DeterministicRng::next_unit() noexcept {
    return static_cast<double>(next_u64() >> 11U) * (1.0 / 9007199254740992.0);
}

} // namespace geoworld::foundation
