#include "geoworld/foundation/thread_pool.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace geoworld::foundation {

struct ThreadPool::Impl {
    explicit Impl(std::uint32_t count) {
        workers.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            workers.emplace_back([this] { worker_loop(); });
        }
    }

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        job_available.notify_all();
        for (std::thread& worker : workers) {
            worker.join();
        }
    }

    // 并发边界：active_workers 计数仍处在本 epoch 任务循环内的线程数；
    // 归 0 表示所有任务不仅被领走而且已执行完，此时才允许提交下一批任务。
    void worker_loop() {
        std::uint64_t seen_epoch = 0;
        std::function<void(std::size_t)> local_job;
        std::size_t local_count = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                job_available.wait(lock, [this, seen_epoch] {
                    return stopping.load(std::memory_order_acquire)
                        || job_epoch.load(std::memory_order_acquire) != seen_epoch;
                });
                if (stopping.load(std::memory_order_acquire)) {
                    return;
                }
                seen_epoch = job_epoch.load(std::memory_order_acquire);
                local_job = job;
                local_count = task_count;
                ++entered;
                active_workers.fetch_add(1, std::memory_order_acq_rel);
            }
            for (;;) {
                const std::size_t task = next_task.fetch_add(1, std::memory_order_relaxed);
                if (task >= local_count) {
                    break;
                }
                local_job(task);
            }
            if (active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lock(mutex);
                job_finished.notify_one();
            }
        }
    }

    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable job_available;
    std::condition_variable job_finished;
    std::function<void(std::size_t)> job;
    std::atomic<std::size_t> next_task{};
    std::atomic<std::uint32_t> active_workers{};
    std::size_t task_count{};
    std::uint32_t entered{};
    std::atomic<std::uint64_t> job_epoch{};
    std::atomic<bool> stopping{false};
};

ThreadPool::ThreadPool(std::uint32_t worker_count)
    : impl_(worker_count == 0 ? nullptr : new Impl(worker_count)) {}

ThreadPool::~ThreadPool() {
    delete impl_;
}

std::uint32_t ThreadPool::worker_count() const noexcept {
    return impl_ == nullptr ? 0
                            : static_cast<std::uint32_t>(impl_->workers.size());
}

void ThreadPool::run(std::size_t task_count,
                     const std::function<void(std::size_t)>& func) {
    if (impl_ == nullptr || task_count == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->job = func;
        impl_->task_count = task_count;
        impl_->next_task.store(0, std::memory_order_relaxed);
        impl_->entered = 0;
        ++impl_->job_epoch;
    }
    impl_->job_available.notify_all();

    // 调用线程同样参与执行；其任务在返回前必然完成。
    for (;;) {
        const std::size_t task = impl_->next_task.fetch_add(1, std::memory_order_relaxed);
        if (task >= task_count) {
            break;
        }
        func(task);
    }

    // 等全部工作线程进场（entered 覆盖所有 worker）且离场（active 归 0），
    // 二者缺一不可：只等归 0 会被尚未进场的 worker 绕过。
    const auto expected = static_cast<std::uint32_t>(impl_->workers.size());
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->job_finished.wait(lock, [this, expected] {
        return impl_->entered == expected
            && impl_->active_workers.load(std::memory_order_acquire) == 0;
    });
    impl_->job = nullptr;
}

} // namespace geoworld::foundation
