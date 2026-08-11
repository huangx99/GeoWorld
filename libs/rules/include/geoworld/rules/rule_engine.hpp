#pragma once

#include "geoworld/rules/event_bus.hpp"
#include "geoworld/rules/expression.hpp"
#include "geoworld/simulation/command_buffer.hpp"
#include "geoworld/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace geoworld::rules {

struct SetPropertyAction {
    foundation::WorldId target;
    std::string key;
    schema::Value value;
};

using RuleAction = std::variant<SetPropertyAction>;

struct Rule {
    std::string id;
    std::uint32_t version{};
    int priority{};
    std::string event_type;
    Expression condition;
    std::vector<RuleAction> actions;
};

enum class RuleRegistrationResult {
    registered,
    invalid_rule,
    duplicate_id
};

class DecisionTable {
public:
    [[nodiscard]] RuleRegistrationResult insert(Rule rule);
    [[nodiscard]] bool erase(std::string_view id);
    [[nodiscard]] const std::vector<Rule>& rows() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::vector<Rule> rows_;
};

struct RuleEvaluationReport {
    std::size_t events_processed{};
    std::size_t rules_fired{};
    std::size_t commands_enqueued{};
};

class RuleEngine {
public:
    [[nodiscard]] RuleRegistrationResult register_rule(Rule rule);
    [[nodiscard]] bool remove_rule(std::string_view id);
    [[nodiscard]] EventBus& events() noexcept;
    [[nodiscard]] const EventBus& events() const noexcept;
    [[nodiscard]] std::uint64_t publish(std::uint64_t target_tick, int priority,
                                         std::string type,
                                         foundation::WorldId subject = {},
                                         schema::PropertyBag payload = {});
    [[nodiscard]] RuleEvaluationReport evaluate(std::uint64_t tick,
                                                simulation::CommandBuffer& commands);
    [[nodiscard]] RuleEvaluationReport evaluate(std::uint64_t tick, world::World& world,
                                                simulation::CommandBuffer& commands);
    [[nodiscard]] std::size_t rule_count() const noexcept;

private:
    EventBus events_;
    DecisionTable table_;
};

} // namespace geoworld::rules
