#include "geoworld/ai/fsm.hpp"
#include "geoworld/ai/lua_runtime.hpp"
#include "geoworld/ai/wasm_runtime.hpp"
#include "geoworld/rules/event_bus.hpp"
#include "geoworld/rules/plugin_registry.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t runtime_iterations = 200'000;
constexpr std::uint64_t plugin_iterations = 100'000;
constexpr std::uint64_t due_tick = 9;
constexpr std::int32_t positive_argument = 42;

int publish_event(void*, const geoworld_rule_event_v1*) { return 0; }
int set_property(void*, std::uint64_t, const char*, const std::uint8_t*, std::uint32_t) { return 0; }

template <typename Function>
std::int64_t measure_nanoseconds(Function&& function) {
    const auto started = std::chrono::steady_clock::now();
    function();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
}

void print_plugin_result(std::string_view track, std::int64_t elapsed,
                         std::uint64_t successful_calls) {
    std::cout << "plugin_track=" << track
              << " calls=" << plugin_iterations
              << " total_ns=" << elapsed
              << " ns_per_call=" << std::fixed << std::setprecision(2)
              << static_cast<double>(elapsed) / static_cast<double>(plugin_iterations)
              << " successful_calls=" << successful_calls << '\n';
}

} // namespace

int main() {
    geoworld::rules::EventBus events;
    std::vector<geoworld::rules::Event> due;
    const auto event_nanoseconds = measure_nanoseconds([&events, &due] {
        for (std::uint64_t index = 0; index < runtime_iterations; ++index) {
            static_cast<void>(events.publish(index % (due_tick + 1),
                                             static_cast<int>(index % 4), "benchmark"));
        }
        due = events.drain(due_tick);
    });

    geoworld::ai::FiniteStateMachine fsm;
    static_cast<void>(fsm.define_state({"idle", {{"toggle", "active", 0}}}));
    static_cast<void>(fsm.define_state({"active", {{"toggle", "idle", 0}}}));
    static_cast<void>(fsm.set_initial("idle"));
    const auto fsm_nanoseconds = measure_nanoseconds([&fsm] {
        for (std::uint64_t index = 0; index < runtime_iterations; ++index) {
            static_cast<void>(fsm.dispatch("toggle"));
        }
    });

    std::cout << "events=" << due.size() << " event_total_ns=" << event_nanoseconds
              << " fsm_transitions=" << runtime_iterations
              << " fsm_total_ns=" << fsm_nanoseconds << '\n';

#if defined(GW_BENCHMARK_RULE_PLUGIN_PATH)
    const geoworld_rule_host_v1 host{
        GEOWORLD_RULE_PLUGIN_ABI_VERSION, nullptr, publish_event, set_property
    };
    geoworld::rules::PluginRegistry native;
    if (!native.load_plugin(GW_BENCHMARK_RULE_PLUGIN_PATH, &host)) {
        std::cerr << "native_plugin_error=" << native.last_error() << '\n';
        return 1;
    }
    const geoworld_rule_event_v1 event{"benchmark", 0, positive_argument, nullptr, 0};
    std::uint64_t native_successes{};
    const auto native_nanoseconds = measure_nanoseconds([&] {
        for (std::uint64_t index = 0; index < plugin_iterations; ++index) {
            native_successes += native.dispatch(event);
        }
    });
    print_plugin_result("native_c_abi", native_nanoseconds, native_successes);
#else
    return 1;
#endif

    geoworld::ai::LuaRuntime lua;
    if (!lua.available()) {
        std::cout << "plugin_track=lua status=unavailable\n";
    } else {
        if (!lua.load("function evaluate(value) return value > 0 end")) {
            return 1;
        }
        std::uint64_t lua_successes{};
        const auto lua_nanoseconds = measure_nanoseconds([&] {
            for (std::uint64_t index = 0; index < plugin_iterations; ++index) {
                lua_successes += lua.call_bool("evaluate", positive_argument) ? 1 : 0;
            }
        });
        print_plugin_result("lua", lua_nanoseconds, lua_successes);
        if (lua_successes != plugin_iterations) {
            return 1;
        }
    }

    geoworld::ai::WasmRuntime wasm;
    if (!wasm.available()) {
        std::cout << "plugin_track=wasm status=unavailable\n";
    } else {
        const std::vector<std::uint8_t> positive_module{
            0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
            0x03, 0x02, 0x01, 0x00,
            0x07, 0x0c, 0x01, 0x08, 0x65, 0x76, 0x61, 0x6c, 0x75, 0x61, 0x74, 0x65, 0x00, 0x00,
            0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x41, 0x00, 0x4a, 0x0b
        };
        if (!wasm.load(positive_module)) {
            return 1;
        }
        std::uint64_t wasm_successes{};
        const auto wasm_nanoseconds = measure_nanoseconds([&] {
            for (std::uint64_t index = 0; index < plugin_iterations; ++index) {
                std::int32_t result{};
                if (wasm.invoke_i32("evaluate", positive_argument, result) && result != 0) {
                    ++wasm_successes;
                }
            }
        });
        print_plugin_result("wasm", wasm_nanoseconds, wasm_successes);
        if (wasm_successes != plugin_iterations) {
            return 1;
        }
    }

    return due.size() == runtime_iterations && fsm.state() == "idle"
        && native_successes == plugin_iterations ? 0 : 1;
}
