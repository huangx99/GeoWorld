#include "geoworld/rules/event_bus.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace geoworld::rules {

std::uint64_t EventBus::publish(std::uint64_t target_tick, int priority,
                                std::string type, foundation::WorldId subject,
                                schema::PropertyBag payload) {
    return schedule(Event{0, target_tick, priority, std::move(type), subject,
                          std::move(payload)});
}

std::uint64_t EventBus::schedule(Event event) {
    event.sequence = next_sequence_++;
    pending_.push_back(std::move(event));
    return pending_.back().sequence;
}

bool EventBus::subscribe(std::string type, Handler handler) {
    if (type.empty() || !handler) {
        return false;
    }
    subscriptions_.push_back({std::move(type), std::move(handler)});
    return true;
}

std::vector<Event> EventBus::drain(std::uint64_t tick) {
    std::stable_sort(pending_.begin(), pending_.end(), [](const Event& left, const Event& right) {
        if (left.target_tick != right.target_tick) {
            return left.target_tick < right.target_tick;
        }
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        return left.sequence < right.sequence;
    });

    std::vector<Event> due;
    auto split = std::stable_partition(pending_.begin(), pending_.end(),
                                       [tick](const Event& event) {
                                           return event.target_tick <= tick;
                                       });
    due.reserve(static_cast<std::size_t>(std::distance(pending_.begin(), split)));
    std::move(pending_.begin(), split, std::back_inserter(due));
    pending_.erase(pending_.begin(), split);
    return due;
}

std::size_t EventBus::dispatch_due(std::uint64_t tick) {
    const auto due = drain(tick);
    std::size_t dispatched{};
    for (const auto& event : due) {
        for (const auto& subscription : subscriptions_) {
            if (subscription.type == event.type || subscription.type == "*") {
                subscription.handler(event);
                ++dispatched;
            }
        }
    }
    return dispatched;
}

std::size_t EventBus::size() const noexcept { return pending_.size(); }

} // namespace geoworld::rules
