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
    // 恢复专用：直接设置下一 tick；dt 由构造配置决定，不在此修改。
    void restore_tick(std::int64_t tick) noexcept { tick_ = tick; }

private:
    TickConfig config_;
    std::int64_t tick_{0};
};

} // namespace geoworld::simulation
