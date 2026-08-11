#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>

namespace geoworld::schema {

using Value = std::variant<std::int64_t, double, bool, std::string>;
using PropertyBag = std::map<std::string, Value>;

} // namespace geoworld::schema
