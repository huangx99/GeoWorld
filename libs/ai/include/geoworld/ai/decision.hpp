#pragma once

#include "geoworld/simulation/command_buffer.hpp"
#include "geoworld/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geoworld::ai {

class DecisionSnapshot {
public:
    [[nodiscard]] static DecisionSnapshot capture(const world::World& world,
                                                   std::uint64_t tick);
    [[nodiscard]] std::uint64_t tick() const noexcept;
    [[nodiscard]] const world::WorldObject* find(foundation::WorldId id) const noexcept;
    [[nodiscard]] const std::vector<world::WorldObject>& objects() const noexcept;

private:
    std::uint64_t tick_{};
    std::vector<world::WorldObject> objects_;
};

struct SetPropertyIntent {
    foundation::WorldId target;
    std::string key;
    schema::Value value;
};

struct IntentFlushReport {
    std::size_t enqueued{};
    std::size_t deferred{};
};

struct PendingIntent {
    std::uint64_t sequence{};
    std::uint64_t target_tick{};
    SetPropertyIntent intent;
};

struct DecisionIntentSnapshot {
    std::uint64_t next_sequence{1};
    std::vector<PendingIntent> pending;
};

class DecisionIntentBuffer {
public:
    [[nodiscard]] bool submit(std::uint64_t target_tick, SetPropertyIntent intent);
    [[nodiscard]] IntentFlushReport flush(std::uint64_t tick,
                                           simulation::CommandBuffer& commands);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] DecisionIntentSnapshot snapshot() const;
    [[nodiscard]] bool restore(DecisionIntentSnapshot snapshot);

private:
    std::uint64_t next_sequence_{1};
    std::vector<PendingIntent> pending_;
};

} // namespace geoworld::ai
