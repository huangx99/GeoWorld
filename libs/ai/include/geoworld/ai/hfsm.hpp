#pragma once

#include "geoworld/ai/fsm.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace geoworld::ai {

struct HierarchicalState {
    std::string id;
    std::string parent;
};

struct HierarchicalTransition {
    std::string source;
    FsmTransition transition;
};

class HierarchicalStateMachine {
public:
    [[nodiscard]] bool define_state(HierarchicalState state);
    [[nodiscard]] bool add_transition(HierarchicalTransition transition);
    [[nodiscard]] bool set_initial(std::string_view state);
    [[nodiscard]] TransitionResult dispatch(std::string_view event);
    [[nodiscard]] std::string_view state() const noexcept;

private:
    [[nodiscard]] const HierarchicalState* find_state(std::string_view id) const noexcept;
    [[nodiscard]] const HierarchicalTransition* transition_for(std::string_view state,
                                                                std::string_view event) const noexcept;

    std::vector<HierarchicalState> states_;
    std::vector<HierarchicalTransition> transitions_;
    std::string current_;
};

} // namespace geoworld::ai
