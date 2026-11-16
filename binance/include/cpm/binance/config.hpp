#pragma once

#include "cpm/binance/reference.hpp"

#include <map>
#include <string>
#include <string_view>

// Where the reference venue is, as a file rather than as literals in the
// source: one WebSocket URL, and one line per instrument saying which
// Binance symbol prices it and what that symbol's tick size is (B-Q15,
// B-Q17).
//
// It holds no credential and never will: this feed signs nothing, asks the
// venue for nothing over HTTP, and could not place an order if it tried.
namespace cpm::binance {

// What a CPM series is priced against.
struct Mapping {
    std::string symbol;   // BTCUSDT
    Price tick_size = 0;  // 0.1, scaled; what PricingMidMoveThreshold counts
};

// Where the venue's REST endpoint is. One GET, public market data, no
// credential: the kline history QuotingMM's volatility and opening price are
// read from when a window's strategy is built (B-Q17's "no REST" reversed,
// the way [MD] Q13's was for the instrument catalog).
struct RestEndpoint {
    std::string host = "fapi.binance.com";
    int port = 443;
};

struct Config {
    ReferenceFeed::Options feed;
    RestEndpoint rest;
    // Keyed by the CPM asset or series - BTCD15M, or BTC - whichever the
    // operator wrote. Both are looked for in a ticker (B-Q7).
    std::map<std::string, Mapping> symbols;
};

// https://fapi.binance.com -> host, port. https:// only, and no path: the
// endpoint paths are the venue's own (/fapi/v1/klines) and are not an
// operator's to spell. Returns an empty string on success, or what is wrong.
std::string parse_rest_url(std::string_view url, RestEndpoint& into);

// wss://fstream.binance.com/ws -> host, port, tls, path. ws:// and wss://
// only; the port defaults to 443 for wss and 80 for ws. Returns an empty
// string on success, or what is wrong with the URL.
std::string parse_ws_url(std::string_view url, ReferenceFeed::Options& into);

// Reads `key = value` lines. Blank lines and `#` comments are ignored; an
// unknown key is an error rather than a shrug, because a typo that is
// quietly ignored is a setting that quietly does not apply.
//
// Returns an empty string on success, or what is wrong with the file,
// naming the line. Takes the text rather than a path so the whole of the
// parsing can be tested without a filesystem.
std::string parse_config(std::string_view text, Config& into);

// The same, from a file. `found` says whether the file was there at all:
// a missing file at the default path is not an error - it means the
// built-in defaults - while a missing file the operator named is.
std::string load_config(const std::string& path, Config& into, bool& found);

}  // namespace cpm::binance
