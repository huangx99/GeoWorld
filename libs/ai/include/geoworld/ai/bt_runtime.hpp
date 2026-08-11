#pragma once

#include <memory>
#include <string>

namespace geoworld::ai {

enum class BehaviorStatus { invalid, idle, running, success, failure };

class BehaviorTreeRuntime {
public:
    explicit BehaviorTreeRuntime(std::string xml);
    ~BehaviorTreeRuntime();

    BehaviorTreeRuntime(BehaviorTreeRuntime&&) noexcept;
    BehaviorTreeRuntime& operator=(BehaviorTreeRuntime&&) noexcept;
    BehaviorTreeRuntime(const BehaviorTreeRuntime&) = delete;
    BehaviorTreeRuntime& operator=(const BehaviorTreeRuntime&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] BehaviorStatus tick();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geoworld::ai
