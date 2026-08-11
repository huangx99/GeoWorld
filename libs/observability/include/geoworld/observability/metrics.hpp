#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace geoworld::observability {

struct TickMetrics {
    std::uint64_t tick{};
    std::uint64_t total_microseconds{};
    std::uint64_t track_a_microseconds{};
    std::uint64_t track_b_microseconds{};
    std::uint64_t track_c_microseconds{};
    std::size_t commands_applied{};
    std::size_t commands_rejected{};
    std::size_t events_processed{};
    bool overloaded{false};
};

class MetricsRecorder {
public:
    using Sink = std::function<void(const TickMetrics&)>;

    void set_sink(Sink sink);
    void record(TickMetrics metrics);
    [[nodiscard]] std::optional<TickMetrics> latest() const noexcept;
    [[nodiscard]] std::uint64_t overload_count() const noexcept;

private:
    std::optional<TickMetrics> latest_;
    std::uint64_t overload_count_{};
    Sink sink_;
};

} // namespace geoworld::observability
