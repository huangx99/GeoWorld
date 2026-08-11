#pragma once

#include "geoworld/foundation/ids.hpp"
#include "geoworld/schema/value.hpp"

#include <cstdint>
#include <string>

namespace geoworld::rules {

struct Event {
    std::uint64_t sequence{};
    std::uint64_t target_tick{};
    int priority{};
    std::string type;
    foundation::WorldId subject;
    schema::PropertyBag payload;
};

} // namespace geoworld::rules
