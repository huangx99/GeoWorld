#include "geoworld/foundation/random.hpp"

#include <utility>

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

bool NamedRandomStreams::create(std::string name, std::uint64_t seed) {
    if (name.empty()) {
        return false;
    }
    return streams_.emplace(std::move(name), DeterministicRng{seed}).second;
}

DeterministicRng* NamedRandomStreams::find(std::string_view name) noexcept {
    const auto found = streams_.find(name);
    return found == streams_.end() ? nullptr : &found->second;
}

const DeterministicRng* NamedRandomStreams::find(std::string_view name) const noexcept {
    const auto found = streams_.find(name);
    return found == streams_.end() ? nullptr : &found->second;
}

std::vector<NamedRandomStreamState> NamedRandomStreams::snapshot() const {
    std::vector<NamedRandomStreamState> result;
    result.reserve(streams_.size());
    for (const auto& [name, stream] : streams_) {
        result.push_back({name, stream.state()});
    }
    return result;
}

bool NamedRandomStreams::restore(std::vector<NamedRandomStreamState> snapshot) {
    std::map<std::string, DeterministicRng, std::less<>> restored;
    for (auto& item : snapshot) {
        if (item.name.empty()
            || !restored.emplace(std::move(item.name), DeterministicRng{item.state}).second) {
            return false;
        }
    }
    streams_ = std::move(restored);
    return true;
}

} // namespace geoworld::foundation
