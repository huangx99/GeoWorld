#include "geoworld/ai/decision.hpp"

#include <algorithm>
#include <utility>

namespace geoworld::ai {

DecisionSnapshot DecisionSnapshot::capture(const world::World& world, std::uint64_t tick) {
    DecisionSnapshot snapshot;
    snapshot.tick_ = tick;
    snapshot.objects_ = world.snapshot();
    return snapshot;
}

std::uint64_t DecisionSnapshot::tick() const noexcept { return tick_; }

const world::WorldObject* DecisionSnapshot::find(foundation::WorldId id) const noexcept {
    const auto iterator = std::lower_bound(objects_.begin(), objects_.end(), id,
                                           [](const auto& object, foundation::WorldId value) {
                                               return object.id < value;
                                           });
    return iterator == objects_.end() || iterator->id != id ? nullptr : &*iterator;
}

const std::vector<world::WorldObject>& DecisionSnapshot::objects() const noexcept {
    return objects_;
}

bool DecisionIntentBuffer::submit(std::uint64_t target_tick, SetPropertyIntent intent) {
    if (!intent.target.valid() || intent.key.empty()) {
        return false;
    }
    pending_.push_back({next_sequence_++, target_tick, std::move(intent)});
    return true;
}

IntentFlushReport DecisionIntentBuffer::flush(std::uint64_t tick,
                                              simulation::CommandBuffer& commands) {
    std::stable_sort(pending_.begin(), pending_.end(), [](const auto& left, const auto& right) {
        return left.sequence < right.sequence;
    });
    IntentFlushReport report;
    std::vector<PendingIntent> deferred;
    deferred.reserve(pending_.size());
    for (auto& pending : pending_) {
        if (pending.target_tick > tick) {
            deferred.push_back(std::move(pending));
            continue;
        }
        static_cast<void>(commands.enqueue(
            pending.target_tick,
            simulation::SetPropertyCommand{
                pending.intent.target, pending.intent.key, pending.intent.value
            }));
        ++report.enqueued;
    }
    report.deferred = deferred.size();
    pending_ = std::move(deferred);
    return report;
}

std::size_t DecisionIntentBuffer::size() const noexcept { return pending_.size(); }

} // namespace geoworld::ai
