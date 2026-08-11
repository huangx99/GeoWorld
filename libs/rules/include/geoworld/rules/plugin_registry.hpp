#pragma once

#include "geoworld/rules/plugin_api.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace geoworld::rules {

class PluginRegistry {
public:
    [[nodiscard]] bool register_plugin(const geoworld_rule_plugin_v1* plugin,
                                       const geoworld_rule_host_v1* host);
    [[nodiscard]] bool unregister_plugin(std::string_view plugin_id);
    [[nodiscard]] std::size_t dispatch(const geoworld_rule_event_v1& event) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Entry {
        std::string id;
        const geoworld_rule_plugin_v1* plugin{};
    };

    std::vector<Entry> plugins_;
};

} // namespace geoworld::rules
