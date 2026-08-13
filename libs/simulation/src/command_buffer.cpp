#include "geoworld/simulation/command_buffer.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace geoworld::simulation {

std::uint64_t CommandBuffer::enqueue(std::uint64_t target_tick, CommandPayload payload) {
    return enqueue(target_tick, std::move(payload), CommandMeta{});
}

std::uint64_t CommandBuffer::enqueue(std::uint64_t target_tick, CommandPayload payload,
                                     CommandMeta meta) {
    const auto sequence = next_sequence_++;
    pending_.push_back(Command{sequence, target_tick, std::move(payload), meta});
    return sequence;
}

ApplyReport CommandBuffer::apply(world::World& world, std::uint64_t tick) {
    ApplyReport report;
    std::vector<Command> deferred;
    deferred.reserve(pending_.size());

    std::stable_sort(pending_.begin(), pending_.end(), [](const auto& left, const auto& right) {
        return left.sequence < right.sequence;
    });

    for (auto& command : pending_) {
        if (command.target_tick > tick) {
            deferred.push_back(std::move(command));
            continue;
        }

        // 乐观并发前置检查在真正 apply 时执行，不在 admission 阶段。
        CommandRejectReason reason = CommandRejectReason::none;
        const auto* set_property = std::get_if<SetPropertyCommand>(&command.payload);
        if (command.meta.expected_object_version != 0 && set_property != nullptr) {
            const world::WorldObject* object = world.find(set_property->id);
            if (object == nullptr) {
                reason = CommandRejectReason::missing_object;
            } else if (object->version != command.meta.expected_object_version) {
                reason = CommandRejectReason::version_conflict;
            }
        }

        bool applied = false;
        if (reason == CommandRejectReason::none) {
            applied = std::visit([&world](auto& payload) {
                using Payload = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, CreateObjectCommand>) {
                    return world.insert(std::move(payload.object));
                } else if constexpr (std::is_same_v<Payload, DestroyObjectCommand>) {
                    return world.erase(payload.id);
                } else {
                    return world.set_property(payload.id, std::move(payload.key),
                                              std::move(payload.value));
                }
            }, command.payload);
            if (!applied) {
                reason = CommandRejectReason::apply_failed;
            }
        }

        applied ? ++report.applied : ++report.rejected;
        report.outcomes.push_back(CommandOutcome{
            command.sequence, command.meta.ingress_sequence, applied, reason});
    }

    report.deferred = deferred.size();
    pending_ = std::move(deferred);
    return report;
}

std::size_t CommandBuffer::size() const noexcept { return pending_.size(); }

} // namespace geoworld::simulation
