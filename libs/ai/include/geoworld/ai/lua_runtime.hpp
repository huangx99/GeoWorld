#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace geoworld::ai {

class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();

    LuaRuntime(LuaRuntime&&) noexcept;
    LuaRuntime& operator=(LuaRuntime&&) noexcept;
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    [[nodiscard]] bool load(std::string_view script);
    [[nodiscard]] bool call_bool(std::string_view function);
    [[nodiscard]] bool available() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geoworld::ai
