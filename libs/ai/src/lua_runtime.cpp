#include "geoworld/ai/lua_runtime.hpp"

#include <utility>

#if GW_HAS_SOL2
#include <sol/sol.hpp>
#endif

namespace geoworld::ai {

struct LuaRuntime::Impl {
#if GW_HAS_SOL2
    sol::state state;

    Impl() {
        state.open_libraries(sol::lib::base, sol::lib::math, sol::lib::table);
    }
#endif
    bool loaded{};
};

LuaRuntime::LuaRuntime() : impl_(std::make_unique<Impl>()) {}
LuaRuntime::~LuaRuntime() = default;
LuaRuntime::LuaRuntime(LuaRuntime&&) noexcept = default;
LuaRuntime& LuaRuntime::operator=(LuaRuntime&&) noexcept = default;

bool LuaRuntime::load(std::string_view script) {
#if GW_HAS_SOL2
    const auto result = impl_->state.safe_script(std::string{script}, sol::script_pass_on_error);
    impl_->loaded = result.valid();
#else
    static_cast<void>(script);
    impl_->loaded = false;
#endif
    return impl_->loaded;
}

bool LuaRuntime::call_bool(std::string_view function) {
#if GW_HAS_SOL2
    if (!impl_->loaded) {
        return false;
    }
    const sol::protected_function callable = impl_->state[std::string{function}];
    if (!callable.valid()) {
        return false;
    }
    const auto result = callable();
    return result.valid() && result.get_type() == sol::type::boolean && result.get<bool>();
#else
    static_cast<void>(function);
    return false;
#endif
}

bool LuaRuntime::available() const noexcept {
#if GW_HAS_SOL2
    return true;
#else
    return false;
#endif
}

} // namespace geoworld::ai
