#include "cpm/binance/config.hpp"

#include "cpm/common/fixed.hpp"

#include <charconv>
#include <fstream>
#include <sstream>

namespace cpm::binance {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Quotes are optional everywhere: the operator's sketch of this file had
// "tickSize" in quotes and the symbol without, and both should work.
std::string_view unquote(std::string_view text) {
    text = trim(text);
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
        text.back() == text.front()) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool to_int(std::string_view text, int& out) {
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, out);
    return result.ec == std::errc() && result.ptr == end;
}

}  // namespace

std::string parse_ws_url(std::string_view url, ReferenceFeed::Options& into) {
    url = trim(url);
    bool tls = true;
    if (lower(url).rfind("wss://", 0) == 0) {
        url.remove_prefix(6);
    } else if (lower(url).rfind("ws://", 0) == 0) {
        tls = false;
        url.remove_prefix(5);
    } else {
        return "a websocket url must start with wss:// or ws://";
    }

    auto authority = url;
    std::string path = "/ws";
    if (const auto slash = url.find('/'); slash != std::string_view::npos) {
        authority = url.substr(0, slash);
        path = std::string(url.substr(slash));
    }
    if (authority.empty()) return "a websocket url needs a host";

    int port = tls ? 443 : 80;
    if (const auto colon = authority.find(':'); colon != std::string_view::npos) {
        const auto text = authority.substr(colon + 1);
        if (!to_int(text, port) || port <= 0 || port > 65535) {
            return "the port in a websocket url must be a port number";
        }
        authority = authority.substr(0, colon);
    }

    into.tls = tls;
    into.host = std::string(authority);
    into.port = port;
    into.path = std::move(path);
    return {};
}

namespace {

// `{BTCUSDT, tickSize: 0.1}`, which is how the operator wrote it, with the
// braces and the quotes optional and `=` accepted for `:`.
std::string parse_mapping(std::string_view value, const std::string& key, Mapping& into) {
    auto body = trim(value);
    if (body.size() >= 2 && body.front() == '{' && body.back() == '}') {
        body = trim(body.substr(1, body.size() - 2));
    }

    bool have_symbol = false;
    std::size_t at = 0;
    while (at <= body.size()) {
        const auto comma = body.find(',', at);
        const auto field = trim(body.substr(at, comma == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : comma - at));
        at = comma == std::string_view::npos ? body.size() + 1 : comma + 1;
        if (field.empty()) continue;

        const auto split = field.find_first_of(":=");
        if (split == std::string_view::npos) {
            if (have_symbol) return "map." + key + " has more than one symbol in it";
            into.symbol = upper(unquote(field));
            have_symbol = true;
            continue;
        }
        const auto name = lower(unquote(field.substr(0, split)));
        const auto setting = unquote(field.substr(split + 1));
        if (name != "ticksize") return "map." + key + " does not know the setting " + name;
        const auto size = cpm::parse_scaled(setting, kScale);
        if (!size || *size <= 0) return "map." + key + "'s tickSize must be a positive decimal";
        into.tick_size = *size;
    }

    if (!have_symbol || into.symbol.empty()) return "map." + key + " needs a Binance symbol";
    if (into.tick_size <= 0) {
        // Not optional, and the reason is worth stating: there is no REST
        // lookup to fall back on any more (B-Q17), and a mapping without a
        // tick size silently disables the guard for that instrument.
        return "map." + key + " needs a tickSize, as in {" + into.symbol + ", tickSize: 0.1}";
    }
    return {};
}

}  // namespace

std::string parse_config(std::string_view text, Config& into) {
    std::istringstream lines{std::string(text)};
    std::string line;
    int number = 0;

    while (std::getline(lines, line)) {
        ++number;
        const auto where = " (line " + std::to_string(number) + ")";
        // A comment may be the whole line or follow a value; a '#' with no
        // space before it is taken as part of the value, so a path or a
        // fragment survives.
        auto content = std::string_view(line);
        if (const auto hash = content.find('#');
            hash == 0 || (hash != std::string_view::npos && hash > 0 &&
                          (content[hash - 1] == ' ' || content[hash - 1] == '\t'))) {
            content = content.substr(0, hash);
        }
        content = trim(content);
        if (content.empty()) continue;

        const auto equals = content.find('=');
        if (equals == std::string_view::npos) {
            return "expected key = value" + where + ": " + std::string(content);
        }
        const auto key = trim(content.substr(0, equals));
        const auto value = trim(content.substr(equals + 1));
        if (key.empty()) return "a value with no key" + where;
        if (value.empty()) return "the key " + std::string(key) + " has no value" + where;

        auto& feed = into.feed;
        if (key == "url") {
            if (const auto failed = parse_ws_url(value, feed); !failed.empty()) {
                return failed + where;
            }
        } else if (key == "ca_file") {
            feed.ca_file = std::string(value);
        } else if (key == "history_file") {
            feed.history_file = std::string(unquote(value));
        } else if (key == "stale_after_ms" || key == "reconnect_after_ms" ||
                   key == "max_reconnect_after_ms" || key == "control_per_sec") {
            int amount = 0;
            if (!to_int(value, amount) || amount < 0) {
                return std::string(key) + " must be a whole number of " +
                       (key == "control_per_sec" ? "messages" : "milliseconds") + where;
            }
            if (key == "stale_after_ms") feed.stale_after = std::chrono::milliseconds(amount);
            else if (key == "reconnect_after_ms") feed.reconnect_after = std::chrono::milliseconds(amount);
            else if (key == "max_reconnect_after_ms") feed.max_reconnect_after = std::chrono::milliseconds(amount);
            else feed.control_per_sec = amount;
        } else if (key == "pin") {
            feed.symbols.push_back(upper(value));
        } else if (key.rfind("map.", 0) == 0) {
            const auto name = upper(key.substr(4));
            if (name.empty()) return "map. needs an instrument after the dot" + where;
            Mapping mapping;
            if (const auto failed = parse_mapping(value, name, mapping); !failed.empty()) {
                return failed + where;
            }
            // A tick size belongs to the Binance symbol, so two instruments
            // that disagree about one are a contradiction, not an override.
            if (const auto known = feed.tick_size.find(mapping.symbol);
                known != feed.tick_size.end() && known->second != mapping.tick_size) {
                return mapping.symbol + " is given two different tick sizes" + where;
            }
            feed.tick_size[mapping.symbol] = mapping.tick_size;
            into.symbols[name] = std::move(mapping);
        } else {
            // Not ignored: a key nobody reads is a setting that silently
            // does not apply, and that is how an operator ends up believing
            // a daemon is pointed somewhere it is not.
            return "unknown setting " + std::string(key) + where;
        }
    }
    return {};
}

std::string load_config(const std::string& path, Config& into, bool& found) {
    found = false;
    std::ifstream file(path);
    if (!file) return {};
    found = true;
    std::ostringstream text;
    text << file.rdbuf();
    if (const auto failed = parse_config(text.str(), into); !failed.empty()) {
        return path + ": " + failed;
    }
    return {};
}

}  // namespace cpm::binance
