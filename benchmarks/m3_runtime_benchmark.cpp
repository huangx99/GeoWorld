#include "geoworld/ai/fsm.hpp"
#include "geoworld/rules/event_bus.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
    constexpr std::uint64_t iterations = 200'000;

    geoworld::rules::EventBus events;
    auto started = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < iterations; ++index) {
        static_cast<void>(events.publish(index % 10, static_cast<int>(index % 4), "benchmark"));
    }
    const auto due = events.drain(9);
    const auto event_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();

    geoworld::ai::FiniteStateMachine fsm;
    static_cast<void>(fsm.define_state({"idle", {{"toggle", "active", 0}}}));
    static_cast<void>(fsm.define_state({"active", {{"toggle", "idle", 0}}}));
    static_cast<void>(fsm.set_initial("idle"));
    started = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < iterations; ++index) {
        static_cast<void>(fsm.dispatch("toggle"));
    }
    const auto fsm_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();

    std::cout << "events=" << due.size() << " event_us=" << event_microseconds
              << " fsm_transitions=" << iterations << " fsm_us=" << fsm_microseconds << '\n';
    return due.size() == iterations && fsm.state() == "idle" ? 0 : 1;
}
