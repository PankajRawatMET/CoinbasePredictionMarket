#include "cpm/strategy/params.hpp"

#include <algorithm>
#include <stdexcept>

namespace cpm::strategy {
namespace {

const ParamSpec* find_spec(const ParamSchema& schema, const std::string& name) {
    const auto it = std::find_if(schema.begin(), schema.end(),
                                 [&](const ParamSpec& spec) { return spec.name == name; });
    return it == schema.end() ? nullptr : &*it;
}

// Parses strictly, in the units the operator typed. Integers may be negative
// (an inventory bound could be), which parse_scaled refuses, so Int is parsed
// here rather than borrowed.
bool parse_int(const std::string& text, std::int64_t& out) {
    if (text.empty()) return false;
    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        negative = text[0] == '-';
        i = 1;
    }
    if (i >= text.size()) return false;
    std::int64_t value = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        // Refuse anything that would wrap rather than silently truncating.
        if (value > (INT64_MAX - (text[i] - '0')) / 10) return false;
        value = value * 10 + (text[i] - '0');
    }
    out = negative ? -value : value;
    return true;
}

bool parse_in_units(const ParamSpec& spec, const std::string& text, std::int64_t& out) {
    if (spec.type == ParamSpec::Type::Decimal) {
        const auto parsed = cpm::parse_scaled(text, spec.scale);
        if (!parsed) return false;
        out = *parsed;
        return true;
    }
    return parse_int(text, out);
}

std::string describe_bounds(const ParamSpec& spec) {
    if (!spec.min.empty() && !spec.max.empty()) return " (" + spec.min + " to " + spec.max + ")";
    if (!spec.min.empty()) return " (at least " + spec.min + ")";
    if (!spec.max.empty()) return " (at most " + spec.max + ")";
    return "";
}

std::string check(const ParamSpec& spec, const std::string& text) {
    switch (spec.type) {
        case ParamSpec::Type::Text:
            if (text.empty()) return spec.name + " must not be empty";
            return {};
        case ParamSpec::Type::Enum:
            if (std::find(spec.choices.begin(), spec.choices.end(), text) == spec.choices.end()) {
                std::string list;
                for (const auto& choice : spec.choices) {
                    if (!list.empty()) list += ", ";
                    list += choice;
                }
                return spec.name + " must be one of: " + list;
            }
            return {};
        case ParamSpec::Type::Int:
        case ParamSpec::Type::Decimal: {
            std::int64_t value = 0;
            if (!parse_in_units(spec, text, value)) {
                // parse_scaled refuses a sign outright, so a negative decimal
                // would otherwise be reported as a precision problem, which
                // sends the operator looking in the wrong place.
                if (spec.type == ParamSpec::Type::Decimal && !text.empty() && text[0] == '-') {
                    return spec.name + " must not be negative" + describe_bounds(spec);
                }
                return spec.name + " must be " +
                       (spec.type == ParamSpec::Type::Int
                            ? "a whole number"
                            : "a decimal with at most " + std::to_string(spec.scale) + " places");
            }
            std::int64_t bound = 0;
            if (!spec.min.empty() && parse_in_units(spec, spec.min, bound) && value < bound) {
                return spec.name + " is too small" + describe_bounds(spec);
            }
            if (!spec.max.empty() && parse_in_units(spec, spec.max, bound) && value > bound) {
                return spec.name + " is too large" + describe_bounds(spec);
            }
            return {};
        }
    }
    return {};
}

}  // namespace

const char* to_string(ParamSpec::Type type) {
    switch (type) {
        case ParamSpec::Type::Int: return "int";
        case ParamSpec::Type::Decimal: return "decimal";
        case ParamSpec::Type::Enum: return "enum";
        case ParamSpec::Type::Text: return "text";
    }
    return "unknown";
}

ParamResult validate(const ParamSchema& schema, const ParamValues& given) {
    ParamResult out;

    // An unknown name is refused rather than ignored: a typo in --with-ticks
    // that silently quoted at the default width is exactly the kind of quiet
    // wrong answer this project keeps refusing to produce.
    for (const auto& [name, value] : given) {
        (void)value;
        if (!find_spec(schema, name)) {
            out.error = "unknown parameter " + name;
            return out;
        }
    }

    for (const auto& spec : schema) {
        const auto it = given.find(spec.name);
        if (it == given.end()) {
            if (spec.required()) {
                out.error = spec.name + " is required";
                return out;
            }
            out.values[spec.name] = spec.default_value;
            continue;
        }
        if (auto error = check(spec, it->second); !error.empty()) {
            out.error = std::move(error);
            return out;
        }
        out.values[spec.name] = it->second;
    }
    return out;
}

std::int64_t param_int(const ParamValues& values, const std::string& name) {
    const auto it = values.find(name);
    if (it == values.end()) throw std::out_of_range("no such parameter: " + name);
    std::int64_t value = 0;
    if (!parse_int(it->second, value)) throw std::out_of_range("parameter is not an integer: " + name);
    return value;
}

std::int64_t param_decimal(const ParamValues& values, const std::string& name, int scale) {
    const auto it = values.find(name);
    if (it == values.end()) throw std::out_of_range("no such parameter: " + name);
    const auto parsed = cpm::parse_scaled(it->second, scale);
    if (!parsed) throw std::out_of_range("parameter is not a decimal: " + name);
    return *parsed;
}

const std::string& param_text(const ParamValues& values, const std::string& name) {
    const auto it = values.find(name);
    if (it == values.end()) throw std::out_of_range("no such parameter: " + name);
    return it->second;
}

}  // namespace cpm::strategy
