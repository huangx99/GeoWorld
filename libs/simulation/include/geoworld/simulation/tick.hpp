#pragma once

#include <cstdint>

#include "geoworld/simulation/command_buffer.hpp"

namespace geoworld::simulation {

struct TickConfig {
    std::int64_t dt_microseconds{20'000};
};

class TickClock {
public:
    explicit TickClock(TickConfig config = {});
    [[nodiscard]] std::int64_t tick() const noexcept;
    [[nodiscard]] std::int64_t simulation_time_microseconds() const noexcept;
    [[nodiscard]] std::int64_t dt_microseconds() const noexcept;
    void advance() noexcept;

private:
    TickConfig config_;
    std::int64_t tick_{0};
};

} // namespace geoworld::simulation
