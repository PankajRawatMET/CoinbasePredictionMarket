#pragma once

#include "cpm/md/instruments.hpp"
#include "cpm/oms/orders.hpp"

namespace cpm::trader {

struct FillRecovery {
    std::vector<cpm::oms::ExecutionReport> fills;
    Qty net = 0;
    std::string error;
};

FillRecovery fetch_fill_history(const cpm::md::RestConfig& config, const cpm::md::HttpsFetch& fetch,
                                const std::string& symbol, const std::string& subaccount);

}  // namespace cpm::trader
