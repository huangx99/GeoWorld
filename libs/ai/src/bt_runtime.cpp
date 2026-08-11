#include "geoworld/ai/bt_runtime.hpp"

#include <exception>
#include <utility>

#if GW_HAS_BEHAVIORTREE_CPP
#include <behaviortree_cpp/bt_factory.h>
#endif

namespace geoworld::ai {

struct BehaviorTreeRuntime::Impl {
    bool valid{};
#if GW_HAS_BEHAVIORTREE_CPP
    BT::BehaviorTreeFactory factory;
    BT::Tree tree;

    explicit Impl(const std::string& xml) {
        try {
            tree = factory.createTreeFromText(xml);
            valid = true;
        } catch (const std::exception&) {
            valid = false;
        }
    }
#else
    explicit Impl(const std::string&) {}
#endif
};

BehaviorTreeRuntime::BehaviorTreeRuntime(std::string xml)
    : impl_(std::make_unique<Impl>(xml)) {}

BehaviorTreeRuntime::~BehaviorTreeRuntime() = default;
BehaviorTreeRuntime::BehaviorTreeRuntime(BehaviorTreeRuntime&&) noexcept = default;
BehaviorTreeRuntime& BehaviorTreeRuntime::operator=(BehaviorTreeRuntime&&) noexcept = default;

bool BehaviorTreeRuntime::valid() const noexcept { return impl_ != nullptr && impl_->valid; }

BehaviorStatus BehaviorTreeRuntime::tick() {
    if (!valid()) {
        return BehaviorStatus::invalid;
    }
#if GW_HAS_BEHAVIORTREE_CPP
    switch (impl_->tree.tickOnce()) {
    case BT::NodeStatus::IDLE:
        return BehaviorStatus::idle;
    case BT::NodeStatus::RUNNING:
        return BehaviorStatus::running;
    case BT::NodeStatus::SUCCESS:
        return BehaviorStatus::success;
    case BT::NodeStatus::FAILURE:
        return BehaviorStatus::failure;
    case BT::NodeStatus::SKIPPED:
        return BehaviorStatus::idle;
    }
#endif
    return BehaviorStatus::invalid;
}

} // namespace geoworld::ai
