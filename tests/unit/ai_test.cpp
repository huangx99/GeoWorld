#include "geoworld/ai/fsm.hpp"
#include "geoworld/ai/hfsm.hpp"
#include "geoworld/ai/bt_runtime.hpp"
#include "geoworld/ai/decision.hpp"
#include "geoworld/ai/lua_runtime.hpp"
#include "geoworld/ai/wasm_runtime.hpp"
#include "geoworld/ai/task_scheduler.hpp"

#include <vector>

int main() {
    using geoworld::ai::FiniteStateMachine;
    using geoworld::ai::FsmState;
    using geoworld::ai::FsmTransition;
    using geoworld::ai::HierarchicalState;
    using geoworld::ai::HierarchicalStateMachine;
    using geoworld::ai::HierarchicalTransition;
    using geoworld::ai::TransitionResult;

    FiniteStateMachine fsm;
    if (!fsm.define_state({"idle", {{"wake", "active", 0}}})
        || !fsm.define_state({"active", {{"stop", "idle", 0}}})
        || !fsm.set_initial("idle")
        || fsm.dispatch("stop") != TransitionResult::ignored
        || fsm.dispatch("wake") != TransitionResult::transitioned
        || fsm.state() != "active") {
        return 1;
    }

    HierarchicalStateMachine hfsm;
    if (!hfsm.define_state({"root", ""})
        || !hfsm.define_state({"root/alert", "root"})
        || !hfsm.define_state({"root/alert/evacuate", "root/alert"})
        || !hfsm.add_transition({"root/alert", {"resume", "root", 0}})
        || !hfsm.add_transition({"root", {"panic", "root/alert", 0}})
        || !hfsm.set_initial("root/alert/evacuate")
        || hfsm.dispatch("resume") != TransitionResult::transitioned
        || hfsm.state() != "root") {
        return 1;
    }

    std::vector<int> execution;
    geoworld::ai::AiTaskScheduler scheduler;
    if (scheduler.submit({{2}, 1, [&execution] { execution.push_back(2); }}) == 0
        || scheduler.submit({{1}, 10, [&execution] { execution.push_back(1); }}) == 0
        || scheduler.submit({{3}, 5, [&execution] { execution.push_back(3); }}) == 0
        || scheduler.run_budget(2).executed != 2
        || execution != std::vector<int>{1, 3}
        || scheduler.run_budget(2).remaining != 0
        || execution != std::vector<int>{1, 3, 2}) {
        return 1;
    }

    geoworld::world::World world;
    geoworld::world::WorldObject agent;
    agent.id = {9};
    agent.semantic_type = "agent";
    agent.properties["energy"] = 50.0;
    if (!world.insert(agent)) {
        return 1;
    }
    const auto snapshot = geoworld::ai::DecisionSnapshot::capture(world, 6);
    if (snapshot.tick() != 6 || snapshot.find({9}) == nullptr
        || snapshot.find({9})->properties.at("energy") != geoworld::schema::Value{50.0}) {
        return 1;
    }
    geoworld::ai::DecisionIntentBuffer intents;
    geoworld::simulation::CommandBuffer commands;
    if (!intents.submit(7, {{9}, "energy", 40.0})
        || intents.flush(6, commands).deferred != 1
        || intents.flush(7, commands).enqueued != 1
        || commands.apply(world, 7).applied != 1
        || std::get<double>(world.find({9})->properties.at("energy")) != 40.0) {
        return 1;
    }

    geoworld::ai::BehaviorTreeRuntime behavior_tree{
        R"(<root BTCPP_format="4" main_tree_to_execute="MainTree">
            <BehaviorTree ID="MainTree"><AlwaysSuccess/></BehaviorTree>
        </root>)"
    };
#if GW_HAS_BEHAVIORTREE_CPP
    if (!behavior_tree.valid()
        || behavior_tree.tick() != geoworld::ai::BehaviorStatus::success) {
        return 1;
    }
#else
    if (behavior_tree.valid()
        || behavior_tree.tick() != geoworld::ai::BehaviorStatus::invalid) {
        return 1;
    }
#endif

    geoworld::ai::LuaRuntime lua;
#if GW_HAS_SOL2
    if (!lua.available()
        || !lua.load("function ready() return true end")
        || !lua.call_bool("ready")) {
        return 1;
    }
#else
    if (lua.available() || lua.load("function ready() return true end")) {
        return 1;
    }
#endif

    geoworld::ai::WasmRuntime wasm;
    const std::vector<std::uint8_t> wasm_module{
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x00,
        0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b
    };
#if GW_HAS_WASMEDGE
    std::int32_t wasm_result{};
    if (!wasm.available() || !wasm.load(wasm_module)
        || !wasm.invoke_i32("run", wasm_result) || wasm_result != 42) {
        return 1;
    }
#else
    if (wasm.available() || wasm.load(wasm_module)) {
        return 1;
    }
#endif
    return 0;
}
