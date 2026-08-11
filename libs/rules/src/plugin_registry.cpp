#include "geoworld/rules/plugin_registry.hpp"

#include <algorithm>

namespace geoworld::rules {

bool PluginRegistry::register_plugin(const geoworld_rule_plugin_v1* plugin,
                                     const geoworld_rule_host_v1* host) {
    if (plugin == nullptr || host == nullptr || plugin->abi_version != GEOWORLD_RULE_PLUGIN_ABI_VERSION
        || host->abi_version != GEOWORLD_RULE_PLUGIN_ABI_VERSION || plugin->plugin_id == nullptr
        || plugin->plugin_id[0] == '\0' || plugin->initialize == nullptr || plugin->on_event == nullptr
        || plugin->shutdown == nullptr) {
        return false;
    }
    if (std::any_of(plugins_.begin(), plugins_.end(), [plugin](const Entry& entry) {
            return entry.id == plugin->plugin_id;
        }) || plugin->initialize(host) != 0) {
        return false;
    }
    plugins_.push_back({plugin->plugin_id, plugin});
    std::sort(plugins_.begin(), plugins_.end(), [](const Entry& left, const Entry& right) {
        return left.id < right.id;
    });
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

} // namespace geoworld::rules
