#include "fill_recovery.hpp"

#include "cpm/common/json.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cpm::trader {
namespace {

using Clock = cpm::oms::Clock;

std::string text_of(const JsonValue& row, const char* field) {
    const auto* value = row.find(field);
    return value && value->type == JsonValue::Type::String ? value->text : std::string();
}

std::string url_encode(const std::string& value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            result += static_cast<char>(ch);
        } else {
            result += '%';
            result += hex[ch >> 4];
            result += hex[ch & 15];
        }
    }
    return result;
}

std::optional<Clock::time_point> timestamp(const std::string& value) {
    if (value.size() < 20 || value.back() != 'Z') return std::nullopt;
    std::tm time{};
    std::istringstream stream(value.substr(0, 19));
    if (!(stream >> std::get_time(&time, "%Y-%m-%dT%H:%M:%S")) || !stream.eof()) {
        return std::nullopt;
    }
    std::int64_t micros = 0;
    if (value.size() > 20) {
        if (value[19] != '.' || value.size() < 22 || value.size() > 30) return std::nullopt;
        const auto fraction = parse_signed_truncating("0" + value.substr(19, value.size() - 20), 6);
        if (!fraction) return std::nullopt;
        micros = *fraction;
    }
    using namespace std::chrono;
    const year_month_day date{year{time.tm_year + 1900},
                              month{static_cast<unsigned>(time.tm_mon + 1)},
                              day{static_cast<unsigned>(time.tm_mday)}};
    if (!date.ok()) return std::nullopt;
    return sys_days{date} + hours{time.tm_hour} + minutes{time.tm_min} + seconds{time.tm_sec} +
           microseconds{micros};
}

JsonValue get(const cpm::md::RestConfig& config, const cpm::md::RestEndpoint& endpoint,
              const cpm::md::HttpsFetch& fetch, const std::string& path, const std::string& query) {
    const auto response = cpm::md::signed_rest_get(config, endpoint, path, query, fetch, Clock::now());
    if (!response.error.empty()) throw std::runtime_error(path + ": " + response.error);
    if (response.status != 200) {
        throw std::runtime_error(path + " returned HTTP " + std::to_string(response.status));
    }
    auto body = json_parse(response.body);
    if (!body.ok() || body.value.type != JsonValue::Type::Object) {
        throw std::runtime_error(path + " returned invalid JSON");
    }
    return std::move(body.value);
}

template<class ReadRow>
void read_pages(const cpm::md::RestConfig& config, const cpm::md::RestEndpoint& endpoint,
                const cpm::md::HttpsFetch& fetch, const std::string& path, const std::string& query,
                ReadRow read_row) {
    std::set<std::string> seen;
    std::string cursor;
    do {
        const auto page = get(config, endpoint, fetch, path,
                              query + (cursor.empty() ? "" : "&cursor=" + url_encode(cursor)));
        const auto* data = page.find("data");
        if (!data || data->type != JsonValue::Type::Array) {
            throw std::runtime_error(path + " returned no data array");
        }
        for (const auto& row : data->items) read_row(row);

        const auto* next = page.find("cursor");
        if (next && next->type != JsonValue::Type::String && next->type != JsonValue::Type::Null) {
            throw std::runtime_error(path + " returned an invalid cursor");
        }
        cursor = text_of(page, "cursor");
        if (!cursor.empty() && !seen.insert(cursor).second) {
            throw std::runtime_error(path + " repeated a pagination cursor");
        }
    } while (!cursor.empty());
}

}  // namespace

FillRecovery fetch_fill_history(const cpm::md::RestConfig& config, const cpm::md::HttpsFetch& fetch,
                                const std::string& symbol, const std::string& subaccount) {
    FillRecovery result;
    try {
        const auto endpoint = cpm::md::parse_rest_url(config.url);
        if (!endpoint.error.empty()) throw std::runtime_error(endpoint.error);
        if (!config.configured() || subaccount.empty()) {
            throw std::runtime_error("configure CPM_REST_* and the FIX sub-account");
        }
        const auto instrument = get(config, endpoint, fetch,
                                    endpoint.prefix + "/instruments/" + url_encode(symbol), "");
        const auto id = text_of(instrument, "id");
        if (id.empty() || text_of(instrument, "ticker") != symbol ||
            text_of(instrument, "status") != "OPEN") {
            throw std::runtime_error("REST could not verify the open instrument");
        }

        // The catalog is under /api/v1, while the account endpoints are under /api.
        const auto account = endpoint.prefix.substr(0, endpoint.prefix.size() - 3);
        const auto query = "instrument_id=" + url_encode(id) + "&sub_account=" +
                           url_encode(subaccount) + "&limit=500";
        read_pages(config, endpoint, fetch, account + "/orders", query, [](const JsonValue&) {
            throw std::runtime_error("working orders exist for this market; cancel them and retry");
        });
        const auto matches = [&](const JsonValue& row) {
            return text_of(row, "instrument_id") == id && text_of(row, "ticker") == symbol &&
                   text_of(row, "sub_account") == subaccount;
        };

        read_pages(config, endpoint, fetch, account + "/fills", query, [&](const JsonValue& row) {
            const auto trade_id = text_of(row, "trade_id");
            const auto order_id = text_of(row, "order_id");
            const auto side = text_of(row, "side");
            const auto role = text_of(row, "role");
            const auto price = parse_signed_truncating(text_of(row, "price"), kPriceScale);
            const auto quantity = parse_qty(text_of(row, "quantity"));
            const auto fee = parse_amount(text_of(row, "fee"));
            const auto time = timestamp(text_of(row, "timestamp"));
            if (!matches(row) || trade_id.empty() || order_id.empty() || order_id == "0" ||
                (side != "BUY" && side != "SELL") || (role != "MAKER" && role != "TAKER") ||
                !price || *price <= 0 || *price >= kPriceOne || !quantity || *quantity <= 0 ||
                !fee || !time) {
                throw std::runtime_error("REST returned an invalid or out-of-scope fill");
            }
            cpm::oms::ExecutionReport report;
            report.exec_type = cpm::oms::ExecType::Trade;
            report.symbol = symbol;
            report.side = side == "BUY" ? cpm::oms::Side::Buy : cpm::oms::Side::Sell;
            report.order_id = order_id;
            report.transact_time = *time;
            report.fill = cpm::oms::Fill{*quantity, *price, trade_id, role == "TAKER", *fee};
            result.fills.push_back(std::move(report));
        });

        bool position_seen = false;
        read_pages(config, endpoint, fetch, account + "/positions", query, [&](const JsonValue& row) {
            const auto qty = parse_signed_truncating(text_of(row, "quantity"), kQtyScale);
            if (position_seen || !matches(row) || !qty) {
                throw std::runtime_error("REST returned an ambiguous or out-of-scope position");
            }
            position_seen = true;
            result.net = *qty;
        });
        std::reverse(result.fills.begin(), result.fills.end());
    } catch (const std::exception& ex) {
        result.error = ex.what();
    }
    return result;
}

}  // namespace cpm::trader
