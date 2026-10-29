#pragma once

#include "cpm/strategy/params.hpp"
#include "cpm/strategy/strategy.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cpm::strategy {

// What the operator asks for: a kind by name and the values for its schema.
struct StrategySpec {
    std::string kind;
    ParamValues params;
};

// The kinds this build knows how to run. A kind registers its name, its
// schema and a factory; nothing else in the system needs to know the kind
// exists - which is the whole point, since it means the GUI renders a form for
// a strategy added after the page was written.
class StrategyRegistry {
public:
    using Factory = std::function<std::unique_ptr<IStrategy>(const ParamValues&, StrategyHooks)>;

    struct Kind {
        std::string name;
        std::string help;
        ParamSchema schema;
        Factory factory;
    };

    // Throws std::invalid_argument on a duplicate name or an empty factory.
    void add(std::string name, std::string help, ParamSchema schema, Factory factory);

    std::vector<std::string> kinds() const;
    const Kind* find(const std::string& name) const;

    struct CreateResult {
        std::unique_ptr<IStrategy> strategy;
        ParamValues values;  // what was used, defaults filled in
        std::string error;

        bool ok() const { return error.empty(); }
    };

    // Validates the spec against the kind's schema and constructs it, or
    // returns the error. Never throws for bad operator input.
    CreateResult create(const StrategySpec& spec, StrategyHooks hooks) const;

private:
    std::vector<Kind> kinds_;
};

}  // namespace cpm::strategy
