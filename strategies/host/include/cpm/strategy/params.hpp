#pragma once

#include "cpm/common/fixed.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// A strategy kind's tunables, described well enough that a form can be
// rendered from them and a value validated against them. One schema serves
// both the GUI and the command line, so the two cannot disagree about what
// --width-ticks 0 means.
namespace cpm::strategy {

struct ParamSpec {
    enum class Type {
        Int,      // a plain integer
        Decimal,  // fixed point at `scale` digits, parsed with parse_scaled
        Enum,     // one of `choices`
        Text,     // a non-empty string, e.g. a symbol
    };

    std::string name;  // the CLI flag without the dashes: "width-ticks"
    // What a screen calls it, when the operator's word for it is not the
    // flag's: "Spread" for width-ticks. Empty means the name title-cased,
    // which is right for most of them. The wire name never moves with it, so
    // a saved command line and the stdin console keep working.
    std::string label;
    Type type = Type::Int;
    // Empty means required: there is nothing sensible to default a symbol to.
    std::string default_value;
    std::string help;
    // Inclusive bounds for Int and Decimal, as text in the same units the
    // operator types. Empty for unbounded.
    std::string min;
    std::string max;
    int scale = 0;                     // Decimal only
    std::vector<std::string> choices;  // Enum only

    bool required() const { return default_value.empty(); }
};

using ParamSchema = std::vector<ParamSpec>;
// Values as they arrive from a form or a command line, before validation.
using ParamValues = std::map<std::string, std::string>;

// Validated values, or the first error worded for whoever typed it.
struct ParamResult {
    ParamValues values;  // every schema name present, defaults filled in
    std::string error;

    bool ok() const { return error.empty(); }
};

// Checks `given` against `schema`: no unknown names, no missing required
// ones, every value parseable and within bounds. Fills in defaults for what
// was omitted, so a caller of the typed readers below never has to ask
// whether a value is there.
ParamResult validate(const ParamSchema& schema, const ParamValues& given);

// Typed reads of an already validated set. They throw std::out_of_range for a
// name the schema does not have, which is a programming error rather than an
// operator error: validate() has already refused anything an operator can do.
std::int64_t param_int(const ParamValues& values, const std::string& name);
std::int64_t param_decimal(const ParamValues& values, const std::string& name, int scale);
const std::string& param_text(const ParamValues& values, const std::string& name);

const char* to_string(ParamSpec::Type type);

}  // namespace cpm::strategy
