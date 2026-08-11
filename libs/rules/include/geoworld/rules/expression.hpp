#pragma once

#include "geoworld/schema/value.hpp"

#include <string>
#include <vector>

namespace geoworld::rules {

enum class Comparison {
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal
};

struct Condition {
    std::string field;
    Comparison comparison{Comparison::equal};
    schema::Value expected;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool matches(const schema::PropertyBag& values) const noexcept;
};

class Expression {
public:
    Expression() = default;
    explicit Expression(std::vector<Condition> all_of);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool evaluate(const schema::PropertyBag& values) const noexcept;
    [[nodiscard]] const std::vector<Condition>& conditions() const noexcept;

private:
    std::vector<Condition> all_of_;
};

} // namespace geoworld::rules
