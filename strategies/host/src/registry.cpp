#include "cpm/strategy/registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cpm::strategy {

void StrategyRegistry::add(std::string name, std::string help, ParamSchema schema,
                           Factory factory) {
    if (name.empty()) throw std::invalid_argument("a strategy kind needs a name");
    if (!factory) throw std::invalid_argument("a strategy kind needs a factory: " + name);
    if (find(name)) throw std::invalid_argument("duplicate strategy kind: " + name);
    kinds_.push_back(Kind{std::move(name), std::move(help), std::move(schema), std::move(factory)});
}

std::vector<std::string> StrategyRegistry::kinds() const {
    std::vector<std::string> names;
    names.reserve(kinds_.size());
    for (const auto& kind : kinds_) names.push_back(kind.name);
    return names;
}

const StrategyRegistry::Kind* StrategyRegistry::find(const std::string& name) const {
    const auto it = std::find_if(kinds_.begin(), kinds_.end(),
                                 [&](const Kind& kind) { return kind.name == name; });
    return it == kinds_.end() ? nullptr : &*it;
}

StrategyRegistry::CreateResult StrategyRegistry::create(const StrategySpec& spec,
                                                        StrategyHooks hooks) const {
    CreateResult out;
    const Kind* kind = find(spec.kind);
    if (!kind) {
        out.error = "unknown strategy kind: " + spec.kind;
        return out;
    }
    auto validated = validate(kind->schema, spec.params);
    if (!validated.ok()) {
        out.error = std::move(validated.error);
        return out;
    }
    try {
        out.strategy = kind->factory(validated.values, std::move(hooks));
    } catch (const std::exception& e) {
        // A factory should validate through its schema, so this is a bug
        // rather than operator error - but it is still reported, not thrown,
        // because it arrives over an HTTP request.
        out.error = std::string("could not build ") + spec.kind + ": " + e.what();
        return out;
    }
    if (!out.strategy) {
        out.error = "could not build " + spec.kind;
        return out;
    }
    out.values = std::move(validated.values);
    return out;
}

}  // namespace cpm::strategy
