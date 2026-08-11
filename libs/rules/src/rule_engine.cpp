#include "geoworld/rules/rule_engine.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace geoworld::rules {

RuleRegistrationResult DecisionTable::insert(Rule rule) {
    if (rule.id.empty() || rule.version == 0 || rule.event_type.empty()
        || !rule.condition.valid()) {
        return RuleRegistrationResult::invalid_rule;
    }
    if (std::any_of(rows_.begin(), rows_.end(), [&rule](const Rule& existing) {
            return existing.id == rule.id;
        })) {
        return RuleRegistrationResult::duplicate_id;
    }
    rows_.push_back(std::move(rule));
    std::stable_sort(rows_.begin(), rows_.end(), [](const Rule& left, const Rule& right) {
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        return left.id < right.id;
    });
    return RuleRegistrationResult::registered;
}

bool DecisionTable::erase(std::string_view id) {
    const auto iterator = std::find_if(rows_.begin(), rows_.end(), [id](const Rule& rule) {
        return rule.id == id;
    });
    if (iterator == rows_.end()) {
        return false;
    }
    rows_.erase(iterator);
    return true;
}

const std::vector<Rule>& DecisionTable::rows() const noexcept { return rows_; }
std::size_t DecisionTable::size() const noexcept { return rows_.size(); }

RuleRegistrationResult RuleEngine::register_rule(Rule rule) {
    return table_.insert(std::move(rule));
}

bool RuleEngine::remove_rule(std::string_view id) { return table_.erase(id); }

EventBus& RuleEngine::events() noexcept { return events_; }
const EventBus& RuleEngine::events() const noexcept { return events_; }

std::uint64_t RuleEngine::publish(std::uint64_t target_tick, int priority,
                                  std::string type, foundation::WorldId subject,
                                  schema::PropertyBag payload) {
    return events_.publish(target_tick, priority, std::move(type), subject, std::move(payload));
}

RuleEvaluationReport RuleEngine::evaluate(std::uint64_t tick,
                                          simulation::CommandBuffer& commands) {
    const auto due = events_.drain(tick);
    RuleEvaluationReport report;
    report.events_processed = due.size();
    for (const auto& event : due) {
        for (const auto& rule : table_.rows()) {
            if (rule.event_type != event.type || !rule.condition.evaluate(event.payload)) {
                continue;
            }
            ++report.rules_fired;
            for (const auto& action : rule.actions) {
                std::visit([&](const auto& typed_action) {
                    using Action = std::decay_t<decltype(typed_action)>;
                    if constexpr (std::is_same_v<Action, SetPropertyAction>) {
                        const auto target = typed_action.target.valid()
                            ? typed_action.target : event.subject;
                        static_cast<void>(commands.enqueue(tick, simulation::SetPropertyCommand{
                            target, typed_action.key, typed_action.value
                        }));
                        ++report.commands_enqueued;
                    }
                }, action);
            }
        }
    }
    return report;
}

RuleEvaluationReport RuleEngine::evaluate(std::uint64_t tick, world::World& world,
                                          simulation::CommandBuffer& commands) {
    static_cast<void>(world);
    return evaluate(tick, commands);
}

std::size_t RuleEngine::rule_count() const noexcept { return table_.size(); }

} // namespace geoworld::rules
