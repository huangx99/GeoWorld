#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace geoworld::ai {

class WasmRuntime {
public:
    WasmRuntime();
    ~WasmRuntime();

    WasmRuntime(WasmRuntime&&) noexcept;
    WasmRuntime& operator=(WasmRuntime&&) noexcept;
    WasmRuntime(const WasmRuntime&) = delete;
    WasmRuntime& operator=(const WasmRuntime&) = delete;

    [[nodiscard]] bool load(std::vector<std::uint8_t> module);
    [[nodiscard]] bool invoke_i32(std::string_view function, std::int32_t& result);
    [[nodiscard]] bool available() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geoworld::ai
