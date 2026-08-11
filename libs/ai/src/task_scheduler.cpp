#include "geoworld/ai/task_scheduler.hpp"

#include <algorithm>
#include <utility>

namespace geoworld::ai {

std::uint64_t AiTaskScheduler::submit(AiTask task) {
    if (!task.execute) {
        return 0;
    }
    const auto sequence = next_sequence_++;
    pending_.push_back({sequence, std::move(task)});
    return sequence;
}

TaskRunReport AiTaskScheduler::run_budget(std::size_t max_tasks) {
    std::stable_sort(pending_.begin(), pending_.end(), [](const auto& left, const auto& right) {
        if (left.task.priority != right.task.priority) {
            return left.task.priority > right.task.priority;
        }
        if (left.task.owner != right.task.owner) {
            return left.task.owner < right.task.owner;
        }
        return left.sequence < right.sequence;
    });
    const auto count = std::min(max_tasks, pending_.size());
    std::vector<QueuedTask> ready;
    ready.reserve(count);
    std::move(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count),
              std::back_inserter(ready));
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count));
    for (auto& task : ready) {
        task.task.execute();
    }
    return {count, pending_.size()};
}

std::size_t AiTaskScheduler::size() const noexcept { return pending_.size(); }

} // namespace geoworld::ai
