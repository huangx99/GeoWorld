#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace geoworld::foundation {

// 固定大小线程池：run 把 [0, task_count) 的下标分发给工作线程与调用线程并发执行，
// 全部完成后才返回。并发边界：调用方必须保证不同下标的任务之间无数据竞争；
// 任务执行顺序不确定，结果的正确性不得依赖任务顺序（确定性由任务内部保证）。
class ThreadPool {
public:
    explicit ThreadPool(std::uint32_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] std::uint32_t worker_count() const noexcept;

    // func 以任务下标为参数被并发调用；task_count 为 0 时直接返回。
    void run(std::size_t task_count, const std::function<void(std::size_t)>& func);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace geoworld::foundation
