#include "cpm/gui/views.hpp"
#include "cpm/strategy/crypto_mm.hpp"
#include "cpm/strategy/quoting_mm.hpp"

#include <gtest/gtest.h>

namespace {

TEST(ModelView, BothStrategyKindsUseTheExistingSchemaEndpoint) {
    cpm::strategy::StrategyRegistry registry;
    cpm::strategy::register_crypto_mm(registry);
    cpm::strategy::register_quoting_mm(registry);
    std::vector<cpm::trader::KindView> kinds;
    for (const auto& name : registry.kinds()) {
        const auto* kind = registry.find(name);
        kinds.push_back({kind->name, kind->help, kind->schema});
    }
    const auto json = cpm::gui::json_parse(cpm::gui::schema_json(kinds));
    ASSERT_TRUE(json.ok());
    const auto* list = json.value.find("kinds");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->items.size(), 2u);
    EXPECT_EQ(list->items[0].find("name")->text, "CryptoMM");
    EXPECT_EQ(list->items[1].find("name")->text, "QuotingMM");
    EXPECT_NE(cpm::gui::schema_json(kinds).find("correction-factor"), std::string::npos);
}

TEST(ModelView, ModelDiagnosticsRoundTripAsTextWithoutKindSpecificFields) {
    cpm::trader::Snapshot snapshot;
    cpm::strategy::StrategyInfo strategy;
    strategy.id = "quotingmm-1";
    strategy.kind = "QuotingMM";
    strategy.status.diagnostics["Fair YES (%)"] = "62.500";
    strategy.status.diagnostics["Model state"] = "waiting \"for inputs\"";
    snapshot.strategies.push_back(strategy);
    const auto json = cpm::gui::json_parse(cpm::gui::snapshot_json(snapshot));
    ASSERT_TRUE(json.ok());
    const auto* list = json.value.find("strategies");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->items.size(), 1u);
    const auto* diagnostics = list->items.front().find("diagnostics");
    ASSERT_NE(diagnostics, nullptr);
    EXPECT_EQ(diagnostics->find("Fair YES (%)")->text, "62.500");
    EXPECT_EQ(diagnostics->find("Model state")->text, "waiting \"for inputs\"");
}

}  // namespace
