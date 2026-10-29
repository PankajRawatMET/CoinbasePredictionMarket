#pragma once

// Moved to shared/common: the REST instrument catalog reads JSON too, and
// market_data cannot depend on gui. The names stay visible here so the GUI's
// own code reads as it did.
#include "cpm/common/json.hpp"

namespace cpm::gui {

using cpm::JsonParse;
using cpm::JsonValue;
using cpm::JsonWriter;
using cpm::json_escape;
using cpm::json_parse;

}  // namespace cpm::gui
