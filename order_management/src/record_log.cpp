#include "cpm/oms/record_log.hpp"

#include "cpm/common/fixed.hpp"
#include "cpm/common/logging.hpp"

#include <optional>
#include <vector>

namespace cpm::oms {
namespace {

// Whole microseconds, and an empty field when there is no number - never a
// zero, which would read as "answered instantly" rather than "not measured".
std::string micros(const std::optional<std::chrono::microseconds>& value) {
    return value ? std::to_string(value->count()) : std::string();
}

std::string when(Clock::time_point stamp) {
    return stamp == Clock::time_point{} ? std::string() : utc_stamp(stamp);
}

std::string yes_no(bool value) { return value ? "Y" : "N"; }

constexpr const char* kFillsHeader =
    "stamp,strategy_id,symbol,side,qty,price,fee,aggressor,ack_latency_us,rest_us,net_after,"
    "cash_after,cum_qty,avg_price,trd_match_id,exec_id,cl_ord_id,order_id,replayed,transact_time";

constexpr const char* kPnlHeader =
    "stamp,symbol,net,cash,fees,fills,mark,realized,unrealized,total";

constexpr const char* kQuotesHeader =
    "stamp,strategy_id,symbol,state,armed,bid_qty,bid,ask,ask_qty,mid,reference_mid,reason";

constexpr const char* kOrdersHeader =
    "stamp,event,strategy_id,symbol,side,qty,price,leaves,cum,latency_us,pending_latency_us,"
    "status,cl_ord_id,orig_cl_ord_id,order_id,reject_reason,cancel_reason,text,replayed,"
    "transact_time";

}  // namespace

RecordLog::RecordLog(const std::string& dir, bool quotes)
    : fills_file_(dir, "fills", "csv"),
      orders_file_(dir, "orders", "csv"),
      pnl_file_(dir, "pnl", "csv"),
      quotes_file_(dir, "quotes", "csv"),
      fills_(fills_file_, kFillsHeader),
      orders_(orders_file_, kOrdersHeader),
      pnl_(pnl_file_, kPnlHeader) {
    if (quotes) quotes_ = std::make_unique<CsvLog>(quotes_file_, kQuotesHeader);
}

std::string RecordLog::open_now(const std::string& stamp) {
    if (const auto failed = fills_.open_now(stamp); !failed.empty()) return failed;
    if (const auto failed = orders_.open_now(stamp); !failed.empty()) return failed;
    if (const auto failed = pnl_.open_now(stamp); !failed.empty()) return failed;
    return quotes_ ? quotes_->open_now(stamp) : std::string();
}

void RecordLog::pnl_row(const Position& position, std::optional<Price> mark, const Pnl& pnl) {
    const auto amount = [](const std::optional<Amount>& value) {
        return value ? format_amount(*value) : std::string();
    };
    pnl_.row(utc_stamp(), {
                              position.symbol,
                              format_signed(position.net, cpm::kQtyScale),
                              format_amount(position.cash),
                              format_amount(position.fees),
                              std::to_string(position.fills),
                              mark ? format_price(*mark) : std::string(),
                              format_amount(pnl.realized),
                              amount(pnl.unrealized),
                              amount(pnl.total),
                          });
}

void RecordLog::quote_row(const Quote& quote) {
    if (!quotes_) return;
    const auto price = [](const std::optional<Price>& value) {
        return value ? format_price(*value) : std::string();
    };
    const auto qty = [](const std::optional<Qty>& value) {
        return value ? format_qty(*value) : std::string();
    };
    quotes_->row(utc_stamp(), {
                                  quote.strategy_id,
                                  quote.symbol,
                                  quote.state,
                                  yes_no(quote.armed),
                                  qty(quote.bid_qty),
                                  price(quote.bid),
                                  price(quote.ask),
                                  qty(quote.ask_qty),
                                  price(quote.mid),
                                  quote.reference_mid,
                                  quote.reason,
                              });
}

void RecordLog::write(const AppliedReport& applied, const std::string& strategy_id) {
    // The stamp the narrative log would have given this line, which is what
    // joins a row to the events around it.
    const std::string stamp = utc_stamp();

    if (applied.kind == AppliedReport::Kind::CancelReject) {
        const auto& reject = applied.reject;
        orders_.row(stamp, {
                               "cancel-reject",
                               strategy_id,
                               applied.order.symbol,
                               // The side of the order it was for, when the
                               // order is one we know about at all.
                               applied.order.cl_ord_id.empty()
                                   ? std::string()
                                   : std::string(to_string(applied.order.side)),
                               "",
                               "",
                               "",
                               "",
                               micros(applied.latency),
                               "",
                               // OrderCancelReject(9) carries no OrdStatus
                               // and no TransactTime: CxlRejReason and the
                               // text are the whole of what it says.
                               "",
                               reject.cl_ord_id,
                               reject.orig_cl_ord_id,
                               reject.order_id,
                               reject.reason ? std::to_string(*reject.reason) : std::string(),
                               "",
                               reject.text,
                               yes_no(reject.replayed),
                               "",
                           });
        return;
    }

    const auto& report = applied.report;
    const auto& order = applied.order;

    if (report.exec_type == ExecType::Trade) {
        const auto& fill = report.fill;
        fills_.row(stamp, {
                              strategy_id,
                              report.symbol,
                              to_string(report.side),
                              fill ? format_qty(fill->quantity) : std::string(),
                              fill ? format_price(fill->price) : std::string(),
                              fill && fill->fee ? format_amount(*fill->fee) : std::string(),
                              fill ? (fill->aggressor ? "taker" : "maker") : "",
                              // The order's own placement latency, carried
                              // here so a fill can be read on its own.
                              micros(order.ack_latency),
                              micros(applied.rested),
                              // Signed: a short YES position is negative,
                              // and format_qty would render it as a flat 0.
                              applied.position
                                  ? format_signed(applied.position->net, cpm::kQtyScale)
                                  : std::string(),
                              applied.position ? format_amount(applied.position->cash)
                                               : std::string(),
                              format_qty(report.cumulative_quantity),
                              format_price(order.average_price),
                              fill ? fill->trade_match_id : std::string(),
                              report.exec_id,
                              order.cl_ord_id.empty() ? report.cl_ord_id : order.cl_ord_id,
                              report.order_id,
                              yes_no(report.replayed),
                              when(report.transact_time),
                          });
        return;
    }

    orders_.row(stamp, {
                           to_string(report.exec_type),
                           strategy_id,
                           report.symbol,
                           to_string(report.side),
                           report.order_quantity ? format_qty(*report.order_quantity)
                                                 : format_qty(order.order_quantity),
                           report.price ? format_price(*report.price) : format_price(order.price),
                           format_qty(report.leaves_quantity),
                           format_qty(report.cumulative_quantity),
                           micros(applied.latency),
                           micros(order.pending_latency),
                           to_string(report.status),
                           report.cl_ord_id,
                           report.orig_cl_ord_id,
                           report.order_id,
                           report.reject_reason ? std::to_string(*report.reject_reason)
                                                : std::string(),
                           report.cancel_reason == CancelReason::None
                               ? std::string()
                               : to_string(report.cancel_reason),
                           report.text,
                           yes_no(report.replayed),
                           when(report.transact_time),
                       });
}

}  // namespace cpm::oms
