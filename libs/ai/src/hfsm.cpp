#include "geoworld/ai/hfsm.hpp"

#include <algorithm>
#include <utility>

namespace geoworld::ai {

bool HierarchicalStateMachine::define_state(HierarchicalState state) {
    if (state.id.empty() || std::any_of(states_.begin(), states_.end(), [&state](const auto& item) {
            return item.id == state.id;
        }) || (!state.parent.empty() && find_state(state.parent) == nullptr)) {
        return false;
    }
    states_.push_back(std::move(state));
    return true;
}

bool HierarchicalStateMachine::add_transition(HierarchicalTransition transition) {
    if (transition.source.empty() || transition.transition.event.empty()
        || transition.transition.target.empty() || find_state(transition.source) == nullptr
        || find_state(transition.transition.target) == nullptr) {
        return false;
    }
    transitions_.push_back(std::move(transition));
    return true;
}

bool HierarchicalStateMachine::set_initial(std::string_view state) {
    if (find_state(state) == nullptr) {
        return false;
    }
    current_ = state;
    return true;
}

TransitionResult HierarchicalStateMachine::dispatch(std::string_view event) {
    auto candidate = current_;
    while (!candidate.empty()) {
        if (const auto* transition = transition_for(candidate, event); transition != nullptr) {
            current_ = transition->transition.target;
            return TransitionResult::transitioned;
        }
        const auto* state = find_state(candidate);
        candidate = state == nullptr ? std::string{} : state->parent;
    }
    return TransitionResult::ignored;
}

std::string_view HierarchicalStateMachine::state() const noexcept { return current_; }

const HierarchicalState* HierarchicalStateMachine::find_state(std::string_view id) const noexcept {
    const auto iterator = std::find_if(states_.begin(), states_.end(), [id](const auto& state) {
        return state.id == id;
    });
    return iterator == states_.end() ? nullptr : &*iterator;
}

const HierarchicalTransition* HierarchicalStateMachine::transition_for(
    std::string_view state, std::string_view event) const noexcept {
    const auto iterator = std::min_element(
        transitions_.begin(), transitions_.end(), [state, event](const auto& left, const auto& right) {
            const auto left_match = left.source == state && left.transition.event == event;
            const auto right_match = right.source == state && right.transition.event == event;
            if (left_match != right_match) {
                return left_match > right_match;
            }
            if (left.transition.priority != right.transition.priority) {
                return left.transition.priority > right.transition.priority;
            }
            return left.transition.target < right.transition.target;
        });
    if (iterator == transitions_.end() || iterator->source != state
        || iterator->transition.event != event) {
        return nullptr;
    }
    return &*iterator;
}

} // namespace geoworld::ai
