#include "geoworld/foundation/version.hpp"
#include "geoworld/foundation/ids.hpp"
#include "geoworld/foundation/random.hpp"
#include "geoworld/foundation/thread_pool.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace {

// 线程池：每个下标任务恰好执行一次，run 返回时全部完成，结果与顺序无关。
[[nodiscard]] bool thread_pool_executes_all_tasks_once() {
    geoworld::foundation::ThreadPool pool{4};
    constexpr std::size_t kTaskCount = 10'000;
    std::vector<std::atomic<std::uint32_t>> hits(kTaskCount);
    std::atomic<std::uint64_t> sum{0};
    for (std::uint32_t round = 0; round < 3; ++round) {
        pool.run(kTaskCount, [&hits, &sum](std::size_t index) {
            hits[index].fetch_add(1, std::memory_order_relaxed);
            sum.fetch_add(static_cast<std::uint64_t>(index), std::memory_order_relaxed);
        });
    }
    for (const auto& hit : hits) {
        if (hit.load(std::memory_order_relaxed) != 3) {
            return false;
        }
    }
    constexpr std::uint64_t kExpectedPerRound =
        static_cast<std::uint64_t>(kTaskCount - 1) * kTaskCount / 2;
    return sum.load(std::memory_order_relaxed) == 3 * kExpectedPerRound;
}

} // namespace

int main() {
    using namespace geoworld::foundation;
    if (major_version != 0 || WorldId{0}.valid() || !WorldId{1}.valid()) {
        return 1;
    }
    if (RuntimeId{4, 2} == RuntimeId{4, 3}) {
        return 1;
    }
    DeterministicRng left{1234};
    DeterministicRng right{1234};
    if (left.next_u64() != right.next_u64()) {
        return 1;
    }
    if (!thread_pool_executes_all_tasks_once()) {
        return 2;
    }
    return 0;
}
