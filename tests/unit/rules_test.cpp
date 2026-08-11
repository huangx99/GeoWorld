#include "geoworld/rules/rule_engine.hpp"
#include "geoworld/rules/plugin_registry.hpp"

#include <cstdint>
#include <vector>

namespace {
int initialized = 0;
int received = 0;
int shutdown = 0;
int initialize(const geoworld_rule_host_v1*) { ++initialized; return 0; }
int on_event(const geoworld_rule_event_v1*) { ++received; return 1; }
void on_shutdown() { ++shutdown; }
const geoworld_rule_plugin_v1 plugin{
    GEOWORLD_RULE_PLUGIN_ABI_VERSION, "test.plugin", initialize, on_event, on_shutdown
};
int publish_event(void*, const geoworld_rule_event_v1*) { return 0; }
int set_property(void*, uint64_t, const char*, const uint8_t*, uint32_t) { return 0; }
}

int main() {
    using geoworld::foundation::WorldId;
    using geoworld::rules::Comparison;
    using geoworld::rules::Condition;
    using geoworld::rules::DecisionTable;
    using geoworld::rules::EventBus;
    using geoworld::rules::Expression;
    using geoworld::rules::Rule;
    using geoworld::rules::RuleEngine;
    using geoworld::rules::RuleRegistrationResult;
    using geoworld::rules::SetPropertyAction;

    EventBus bus;
    std::vector<std::uint64_t> order;
    if (!bus.subscribe("alarm", [&order](const auto& event) {
            order.push_back(event.sequence);
        })) {
        return 1;
    }
    const auto first = bus.publish(4, 1, "alarm");
    const auto second = bus.publish(4, 9, "alarm");
    const auto future = bus.publish(5, 100, "alarm");
    if (first == 0 || second == 0 || future == 0 || bus.dispatch_due(3) != 0
        || bus.dispatch_due(4) != 2 || order != std::vector<std::uint64_t>{second, first}
        || bus.size() != 1 || bus.drain(5).size() != 1) {
        return 1;
    }

    RuleEngine engine;
    const auto condition = Expression{{Condition{"temperature", Comparison::greater, 80.0}}};
    Rule rule{
        "overheat-alarm", 1, 10, "temperature.changed", condition,
        {SetPropertyAction{{}, "alarm", true}}
    };
    DecisionTable table;
    if (table.insert(rule) != RuleRegistrationResult::registered || table.size() != 1
        || !table.erase("overheat-alarm") || table.size() != 0) {
        return 1;
    }
    if (engine.register_rule(rule) != RuleRegistrationResult::registered
        || engine.register_rule(rule) != RuleRegistrationResult::duplicate_id) {
        return 1;
    }

    geoworld::world::World world;
    geoworld::world::WorldObject object;
    object.id = WorldId{7};
    object.semantic_type = "device";
    if (!world.insert(object)) {
        return 1;
    }
    geoworld::simulation::CommandBuffer commands;
    static_cast<void>(engine.publish(2, 0, "temperature.changed", WorldId{7},
                                     {{"temperature", 85.0}}));
    if (engine.evaluate(1, world, commands).events_processed != 0
        || engine.evaluate(2, world, commands).rules_fired != 1
        || commands.apply(world, 2).applied != 1) {
        return 1;
    }
    const auto* updated = world.find(WorldId{7});
    if (updated == nullptr || !std::get<bool>(updated->properties.at("alarm"))) {
        return 1;
    }
    geoworld::rules::PluginRegistry plugins;
    const geoworld_rule_host_v1 host{
        GEOWORLD_RULE_PLUGIN_ABI_VERSION, nullptr, publish_event, set_property
    };
    const geoworld_rule_event_v1 event{"test", 2, 7, nullptr, 0};
    if (!plugins.register_plugin(&plugin, &host) || initialized != 1 || plugins.size() != 1
        || plugins.dispatch(event) != 1 || received != 1
        || !plugins.unregister_plugin("test.plugin") || shutdown != 1) {
        return 1;
    }
    return 0;
}
