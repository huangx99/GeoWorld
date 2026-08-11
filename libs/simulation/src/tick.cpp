#include "geoworld/simulation/tick.hpp"

namespace geoworld::simulation {

TickClock::TickClock(TickConfig config) : config_(config) {}

std::int64_t TickClock::tick() const noexcept { return tick_; }

std::int64_t TickClock::simulation_time_microseconds() const noexcept {
    return tick_ * config_.dt_microseconds;
}

std::int64_t TickClock::dt_microseconds() const noexcept { return config_.dt_microseconds; }

void TickClock::advance() noexcept { ++tick_; }

} // namespace geoworld::simulation
