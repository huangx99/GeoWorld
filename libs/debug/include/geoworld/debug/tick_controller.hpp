#pragma once

#include <cstdint>
#include <optional>

namespace geoworld::debug {

class TickController {
public:
    void pause() noexcept;
    void resume() noexcept;
    void step(std::uint64_t count = 1) noexcept;
    void run_to(std::uint64_t tick) noexcept;
    [[nodiscard]] bool should_advance(std::uint64_t current_tick) noexcept;

private:
    bool paused_{false};
    std::uint64_t pending_steps_{};
    std::optional<std::uint64_t> target_tick_;
};

} // namespace geoworld::debug
