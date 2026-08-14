#pragma once

#include "geoworld/rules/event.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace geoworld::rules {

struct EventBusSnapshot {
    std::uint64_t next_sequence{1};
    std::vector<Event> pending;
};

class EventBus {
public:
    using Handler = std::function<void(const Event&)>;

    [[nodiscard]] std::uint64_t publish(std::uint64_t target_tick, int priority,
                                        std::string type,
                                        foundation::WorldId subject = {},
                                        schema::PropertyBag payload = {});
    [[nodiscard]] std::uint64_t schedule(Event event);
    [[nodiscard]] bool subscribe(std::string type, Handler handler);
    [[nodiscard]] std::vector<Event> drain(std::uint64_t tick);
    [[nodiscard]] std::size_t dispatch_due(std::uint64_t tick);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] EventBusSnapshot snapshot() const;
    [[nodiscard]] bool restore(EventBusSnapshot snapshot);

private:
    struct Subscription {
        std::string type;
        Handler handler;
    };

    std::uint64_t next_sequence_{1};
    std::vector<Event> pending_;
    std::vector<Subscription> subscriptions_;
};

} // namespace geoworld::rules
