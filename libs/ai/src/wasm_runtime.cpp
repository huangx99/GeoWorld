#include "geoworld/ai/wasm_runtime.hpp"

#include <utility>

#if GW_HAS_WASMEDGE
#include <wasmedge/wasmedge.h>
#endif

namespace geoworld::ai {

struct WasmRuntime::Impl {
#if GW_HAS_WASMEDGE
    WasmEdge_VMContext* vm{WasmEdge_VMCreate(nullptr, nullptr)};
#endif
    bool loaded{};

    ~Impl() {
#if GW_HAS_WASMEDGE
        if (vm != nullptr) {
            WasmEdge_VMDelete(vm);
        }
#endif
    }
};

WasmRuntime::WasmRuntime() : impl_(std::make_unique<Impl>()) {}
WasmRuntime::~WasmRuntime() = default;
WasmRuntime::WasmRuntime(WasmRuntime&&) noexcept = default;
WasmRuntime& WasmRuntime::operator=(WasmRuntime&&) noexcept = default;

bool WasmRuntime::load(std::vector<std::uint8_t> module) {
#if GW_HAS_WASMEDGE
    if (impl_->vm == nullptr || module.empty()) {
        return false;
    }
    const auto loaded = WasmEdge_VMLoadWasmFromBuffer(impl_->vm, module.data(),
                                                       static_cast<std::uint32_t>(module.size()));
    if (!WasmEdge_ResultOK(loaded)
        || !WasmEdge_ResultOK(WasmEdge_VMValidate(impl_->vm))
        || !WasmEdge_ResultOK(WasmEdge_VMInstantiate(impl_->vm))) {
        impl_->loaded = false;
        return false;
    }
    impl_->loaded = true;
    return true;
#else
    static_cast<void>(module);
    impl_->loaded = false;
    return false;
#endif
}

bool WasmRuntime::invoke_i32(std::string_view function, std::int32_t& result) {
#if GW_HAS_WASMEDGE
    if (!impl_->loaded || function.empty()) {
        return false;
    }
    const auto name = WasmEdge_StringCreateByBuffer(function.data(),
                                                     static_cast<std::uint32_t>(function.size()));
    WasmEdge_Value output{};
    const auto status = WasmEdge_VMExecute(impl_->vm, name, nullptr, 0, &output, 1);
    WasmEdge_StringDelete(name);
    if (!WasmEdge_ResultOK(status) || output.Type != WasmEdge_ValType_I32) {
        return false;
    }
    result = WasmEdge_ValueGetI32(output);
    return true;
#else
    static_cast<void>(function);
    static_cast<void>(result);
    return false;
#endif
}

bool WasmRuntime::available() const noexcept {
#if GW_HAS_WASMEDGE
    return impl_ != nullptr && impl_->vm != nullptr;
#else
    return false;
#endif
}

} // namespace geoworld::ai
