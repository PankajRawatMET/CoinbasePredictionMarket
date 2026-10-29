#pragma once

#include "cpm/gui/json.hpp"
#include "cpm/common/log_ring.hpp"
#include "cpm/trader/control_api.hpp"

#include <string>
#include <vector>

// Everything the daemon says, as JSON. Kept apart from the socket so the shape
// of the API is a unit test rather than something only curl can see.
namespace cpm::gui {

std::string snapshot_json(const cpm::trader::Snapshot& snapshot);
std::string schema_json(const std::vector<cpm::trader::KindView>& kinds);
// One instrument's whole price axis. Asked for once per symbol, because it
// does not change while the instrument exists.
std::string axis_json(const cpm::trader::AxisView& axis);
// {"ok":true,...} for an action that was taken, with the id when create made one.
std::string result_json(const cpm::trader::Result& result, std::uint64_t revision);

// The log lines a page has not been sent yet (GUI-Q27).
std::string logs_json(const std::vector<cpm::LogRing::Line>& lines);
std::string error_json(std::string_view message);

}  // namespace cpm::gui
