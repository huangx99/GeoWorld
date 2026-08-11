#include "geoworld/simulation/command_buffer.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace geoworld::simulation {

std::uint64_t CommandBuffer::enqueue(std::uint64_t target_tick, CommandPayload payload) {
    const auto sequence = next_sequence_++;
    pending_.push_back(Command{sequence, target_tick, std::move(payload)});
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

        const bool applied = std::visit([&world](auto& payload) {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, CreateObjectCommand>) {
                return world.insert(std::move(payload.object));
            } else if constexpr (std::is_same_v<Payload, DestroyObjectCommand>) {
                return world.erase(payload.id);
            } else {
                return world.set_property(payload.id, std::move(payload.key), std::move(payload.value));
            }
        }, command.payload);

        applied ? ++report.applied : ++report.rejected;
    }

    report.deferred = deferred.size();
    pending_ = std::move(deferred);
    return report;
}

std::size_t CommandBuffer::size() const noexcept { return pending_.size(); }

} // namespace geoworld::simulation
