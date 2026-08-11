#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace geoworld::ai {

struct FsmTransition {
    std::string event;
    std::string target;
    int priority{};
};

struct FsmState {
    std::string id;
    std::vector<FsmTransition> transitions;
};

enum class TransitionResult { ignored, transitioned };

class FiniteStateMachine {
public:
    [[nodiscard]] bool define_state(FsmState state);
    [[nodiscard]] bool set_initial(std::string_view state);
    [[nodiscard]] TransitionResult dispatch(std::string_view event);
    [[nodiscard]] std::string_view state() const noexcept;

private:
    std::vector<FsmState> states_;
    std::string current_;
};

} // namespace geoworld::ai
