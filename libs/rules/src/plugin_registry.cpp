#include "geoworld/rules/plugin_registry.hpp"

#include <algorithm>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

void* open_library(const std::string& path, std::string& error) {
#if defined(_WIN32)
    auto* handle = LoadLibraryA(path.c_str());
    if (handle == nullptr) {
        error = "无法加载动态库，系统错误码 " + std::to_string(GetLastError());
    }
    return handle;
#else
    dlerror();
    auto* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* message = dlerror();
        error = message == nullptr ? "无法加载动态库" : message;
    }
    return handle;
#endif
}

void close_library(void* handle) {
    if (handle == nullptr) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

geoworld_rule_plugin_entry_v1 find_entry(void* handle, std::string& error) {
#if defined(_WIN32)
    const auto symbol = GetProcAddress(static_cast<HMODULE>(handle),
                                       GEOWORLD_RULE_PLUGIN_ENTRY_SYMBOL);
    if (symbol == nullptr) {
        error = "动态库缺少入口符号 " GEOWORLD_RULE_PLUGIN_ENTRY_SYMBOL;
        return nullptr;
    }
    return reinterpret_cast<geoworld_rule_plugin_entry_v1>(symbol);
#else
    dlerror();
    void* symbol = dlsym(handle, GEOWORLD_RULE_PLUGIN_ENTRY_SYMBOL);
    if (const char* message = dlerror(); message != nullptr) {
        error = message;
        return nullptr;
    }
    return reinterpret_cast<geoworld_rule_plugin_entry_v1>(symbol);
#endif
}

} // namespace

namespace geoworld::rules {

PluginRegistry::~PluginRegistry() {
    for (auto iterator = plugins_.rbegin(); iterator != plugins_.rend(); ++iterator) {
        iterator->plugin->shutdown();
        close_library(iterator->library_handle);
    }
}

bool PluginRegistry::register_plugin(const geoworld_rule_plugin_v1* plugin,
                                     const geoworld_rule_host_v1* host) {
    last_error_.clear();
    if (plugin == nullptr || host == nullptr || plugin->abi_version != GEOWORLD_RULE_PLUGIN_ABI_VERSION
        || host->abi_version != GEOWORLD_RULE_PLUGIN_ABI_VERSION || plugin->plugin_id == nullptr
        || plugin->plugin_id[0] == '\0' || plugin->initialize == nullptr || plugin->on_event == nullptr
        || plugin->shutdown == nullptr) {
        last_error_ = "插件描述符或宿主 ABI 无效";
        return false;
    }
    if (std::any_of(plugins_.begin(), plugins_.end(), [plugin](const Entry& entry) {
            return entry.id == plugin->plugin_id;
        })) {
        last_error_ = "插件 ID 已注册";
        return false;
    }
    if (plugin->initialize(host) != 0) {
        last_error_ = "插件初始化失败";
        return false;
    }
    plugins_.push_back({plugin->plugin_id, plugin, nullptr});
    std::sort(plugins_.begin(), plugins_.end(), [](const Entry& left, const Entry& right) {
        return left.id < right.id;
    });
    return true;
}

bool PluginRegistry::load_plugin(std::string_view library_path,
                                 const geoworld_rule_host_v1* host) {
    last_error_.clear();
    if (library_path.empty()) {
        last_error_ = "动态库路径为空";
        return false;
    }
    void* handle = open_library(std::string{library_path}, last_error_);
    if (handle == nullptr) {
        return false;
    }
    const auto entry = find_entry(handle, last_error_);
    if (entry == nullptr) {
        close_library(handle);
        return false;
    }
    const geoworld_rule_plugin_v1* plugin = entry();
    if (!register_plugin(plugin, host)) {
        close_library(handle);
        return false;
    }
    const auto iterator = std::find_if(plugins_.begin(), plugins_.end(), [plugin](const Entry& item) {
        return item.plugin == plugin;
    });
    if (iterator == plugins_.end()) {
        last_error_ = "插件注册后无法关联动态库句柄";
        close_library(handle);
        return false;
    }
    iterator->library_handle = handle;
    return true;
}

bool PluginRegistry::unregister_plugin(std::string_view plugin_id) {
    const auto iterator = std::find_if(plugins_.begin(), plugins_.end(), [plugin_id](const Entry& entry) {
        return entry.id == plugin_id;
    });
    if (iterator == plugins_.end()) {
        return false;
    }
    iterator->plugin->shutdown();
    close_library(iterator->library_handle);
    plugins_.erase(iterator);
    return true;
}

std::size_t PluginRegistry::dispatch(const geoworld_rule_event_v1& event) const {
    std::size_t dispatched{};
    for (const auto& entry : plugins_) {
        if (entry.plugin->on_event(&event) != 0) {
            ++dispatched;
        }
    }
    return dispatched;
}

std::size_t PluginRegistry::size() const noexcept { return plugins_.size(); }

std::string_view PluginRegistry::last_error() const noexcept { return last_error_; }

} // namespace geoworld::rules
