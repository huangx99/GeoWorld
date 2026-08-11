#include "geoworld/rules/expression.hpp"

#include <cmath>
#include <utility>

namespace geoworld::rules {
namespace {

bool numeric_value(const schema::Value& value, double& result) noexcept {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        result = static_cast<double>(*integer);
        return true;
    }
    if (const auto* real = std::get_if<double>(&value)) {
        result = *real;
        return std::isfinite(result);
    }
    return false;
}

bool compare(const schema::Value& left, const schema::Value& right,
             Comparison comparison) noexcept {
    if (comparison == Comparison::equal || comparison == Comparison::not_equal) {
        bool equal = false;
        if (left.index() == right.index()) {
            equal = left == right;
        } else {
            double left_number{};
            double right_number{};
            equal = numeric_value(left, left_number) && numeric_value(right, right_number)
                && left_number == right_number;
        }
        return comparison == Comparison::equal ? equal : !equal;
    }

    double left_number{};
    double right_number{};
    if (!numeric_value(left, left_number) || !numeric_value(right, right_number)) {
        return false;
    }
    switch (comparison) {
    case Comparison::less:
        return left_number < right_number;
    case Comparison::less_equal:
        return left_number <= right_number;
    case Comparison::greater:
        return left_number > right_number;
    case Comparison::greater_equal:
        return left_number >= right_number;
    case Comparison::equal:
    case Comparison::not_equal:
        return false;
    }
    return false;
}

} // namespace

bool Condition::valid() const noexcept { return !field.empty(); }

bool Condition::matches(const schema::PropertyBag& values) const noexcept {
    const auto iterator = values.find(field);
    return iterator != values.end() && compare(iterator->second, expected, comparison);
}

Expression::Expression(std::vector<Condition> all_of) : all_of_(std::move(all_of)) {}

bool Expression::valid() const noexcept {
    for (const auto& condition : all_of_) {
        if (!condition.valid()) {
            return false;
        }
    }
    return true;
}

bool Expression::evaluate(const schema::PropertyBag& values) const noexcept {
    if (!valid()) {
        return false;
    }
    for (const auto& condition : all_of_) {
        if (!condition.matches(values)) {
            return false;
        }
    }
    return true;
}

const std::vector<Condition>& Expression::conditions() const noexcept { return all_of_; }

} // namespace geoworld::rules
