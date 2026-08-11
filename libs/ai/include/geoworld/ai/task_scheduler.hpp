#pragma once

#include "geoworld/foundation/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace geoworld::ai {

struct AiTask {
    foundation::WorldId owner;
    int priority{};
    std::function<void()> execute;
};

struct TaskRunReport {
    std::size_t executed{};
    std::size_t remaining{};
};

class AiTaskScheduler {
public:
    [[nodiscard]] std::uint64_t submit(AiTask task);
    [[nodiscard]] TaskRunReport run_budget(std::size_t max_tasks);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct QueuedTask {
        std::uint64_t sequence{};
        AiTask task;
    };

    std::uint64_t next_sequence_{1};
    std::vector<QueuedTask> pending_;
};

} // namespace geoworld::ai
