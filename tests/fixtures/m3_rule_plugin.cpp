#include "geoworld/rules/plugin_api.h"

namespace {

bool initialized{};

int initialize(const geoworld_rule_host_v1* host) {
    initialized = host != nullptr && host->abi_version == GEOWORLD_RULE_PLUGIN_ABI_VERSION;
    return initialized ? 0 : 1;
}

int on_event(const geoworld_rule_event_v1* event) {
    return initialized && event != nullptr && event->subject_id > 0 ? 1 : 0;
}

void shutdown() { initialized = false; }

const geoworld_rule_plugin_v1 plugin{
    GEOWORLD_RULE_PLUGIN_ABI_VERSION,
    "geoworld.test.positive-subject",
    initialize,
    on_event,
    shutdown
};

} // namespace

extern "C" GEOWORLD_RULE_PLUGIN_EXPORT const geoworld_rule_plugin_v1*
geoworld_rule_plugin_entry() {
    return &plugin;
}
