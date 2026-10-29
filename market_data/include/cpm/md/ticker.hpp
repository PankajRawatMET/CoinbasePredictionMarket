#pragma once

#include "cpm/md/events.hpp"

#include <optional>
#include <string>
#include <string_view>

// A rolling series instrument's name, taken apart.
//
//   BTCD15M-BTC26OCT260630-0
//   |       |  |  |  | |    `- suffix
//   |       |  |  |  | `------ HHMM, the window END in UTC
//   |       |  |  |  `-------- DD
//   |       |  |  `----------- MON
//   |       |  `-------------- YY
//   |       `----------------- asset
//   `------------------------- series
//
// The time in the name is the window's end, not its start ([MD] O2, O6). That
// is the fact the whole roll turns on: a 15-minute window announces when it
// will stop being tradable the moment it is named, so nothing has to wait to
// be told.
namespace cpm::md {

struct Ticker {
    std::string series;  // "BTCD15M"
    std::string asset;   // "BTC"
    std::string suffix;  // "0"
    Clock::time_point window_end{};
};

// Nullopt for any name that is not a dated window - a sports market
// (NFLGAME-26OCT25LVLAR-LV), a simulator instrument, anything whose shape does
// not match. A caller that cannot parse a name knows only that it has no
// scheduled end, which is the honest answer for those.
std::optional<Ticker> parse_ticker(std::string_view symbol);

// True if `symbol` could name an instrument at all. A bare series name -
// BTCD15M - cannot: every instrument the venue lists carries its series, its
// event and its outcome, separated by dashes, and a request for a series name
// comes back "unknown symbol", which halts the daemon (Q27).
//
// This is deliberately a shape check and not a lookup. Nothing on either FIX
// session can be asked whether a symbol exists, so the only thing that can be
// caught before the gateway answers is a name that could never be one.
bool looks_like_instrument(std::string_view symbol);

}  // namespace cpm::md
