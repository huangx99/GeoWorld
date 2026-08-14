#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace geoworld::foundation {

class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next_u64() noexcept;
    [[nodiscard]] double next_unit() noexcept;
    [[nodiscard]] std::uint64_t state() const noexcept { return state_; }
    void restore_state(std::uint64_t state) noexcept { state_ = state; }

private:
    std::uint64_t state_;
};

struct NamedRandomStreamState {
    std::string name;
    std::uint64_t state{};
};

class NamedRandomStreams {
public:
    [[nodiscard]] bool create(std::string name, std::uint64_t seed);
    [[nodiscard]] DeterministicRng* find(std::string_view name) noexcept;
    [[nodiscard]] const DeterministicRng* find(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<NamedRandomStreamState> snapshot() const;
    [[nodiscard]] bool restore(std::vector<NamedRandomStreamState> snapshot);

private:
    std::map<std::string, DeterministicRng, std::less<>> streams_;
};

} // namespace geoworld::foundation
