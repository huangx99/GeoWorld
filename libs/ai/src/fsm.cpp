#include "geoworld/ai/fsm.hpp"

#include <algorithm>
#include <utility>

namespace geoworld::ai {

bool FiniteStateMachine::define_state(FsmState state) {
    if (state.id.empty() || std::any_of(states_.begin(), states_.end(), [&state](const auto& item) {
            return item.id == state.id;
        })) {
        return false;
    }
    states_.push_back(std::move(state));
    return true;
}

bool FiniteStateMachine::set_initial(std::string_view state) {
    if (std::none_of(states_.begin(), states_.end(), [state](const auto& item) {
            return item.id == state;
        })) {
        return false;
    }
    current_ = state;
    return true;
}

TransitionResult FiniteStateMachine::dispatch(std::string_view event) {
    const auto state = std::find_if(states_.begin(), states_.end(), [this](const auto& item) {
        return item.id == current_;
    });
    if (state == states_.end()) {
        return TransitionResult::ignored;
    }
    const auto transition = std::min_element(
        state->transitions.begin(), state->transitions.end(), [event](const auto& left, const auto& right) {
            const auto left_match = left.event == event;
            const auto right_match = right.event == event;
            if (left_match != right_match) {
                return left_match > right_match;
            }
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            return left.target < right.target;
        });
    if (transition == state->transitions.end() || transition->event != event
        || std::none_of(states_.begin(), states_.end(), [&transition](const auto& item) {
               return item.id == transition->target;
           })) {
        return TransitionResult::ignored;
    }
    current_ = transition->target;
    return TransitionResult::transitioned;
}

std::string_view FiniteStateMachine::state() const noexcept { return current_; }

} // namespace geoworld::ai
