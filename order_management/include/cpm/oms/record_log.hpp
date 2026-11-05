#pragma once

#include "cpm/common/csv_log.hpp"
#include "cpm/oms/order_state.hpp"
#include "cpm/oms/pnl.hpp"

#include <memory>
#include <optional>
#include <string>

namespace cpm::oms {

// The records half of the logging (docs/design-logs.md): what we did, at what
// price, how long the venue took to answer, and what it made.
//
//     <dir>/fills-YYYYMMDD.csv    one row per fill
//     <dir>/orders-YYYYMMDD.csv   every other report, and each cancel reject
//     <dir>/pnl-YYYYMMDD.csv      every position, on a slow interval
//     <dir>/quotes-YYYYMMDD.csv   each change in what a strategy is showing
//
// The narrative log says what happened and why; these say what it was, in a
// shape that opens in a spreadsheet or a dataframe with no parser written
// first. One row per event, fixed columns, append-only, rolling over daily
// beside the .log files.
//
// A fill is not an order being added, so a fill row carries the latency of
// the order it belongs to rather than one of its own, which is what lets a
// fill be read without joining to orders.csv.
class RecordLog {
public:
    // `quotes` opens the quotes file as well. It is off by default because a
    // market maker changes its quotes about once a second per strategy,
    // while a fill or a P&L row is worth keeping from every run.
    // `run` names this run's directory inside the day's, so a run's records
    // sit beside its logs rather than appended to the day's (see DailyFile).
    explicit RecordLog(const std::string& dir, bool quotes = false, const std::string& run = {});

    // One strategy's quoting decision, as the daemon sees it from outside:
    // what is resting, what the book and the reference venue said at the
    // time, and why it is not quoting if it is not.
    struct Quote {
        std::string strategy_id;
        std::string symbol;
        std::string state;  // Running, Stopping, Stopped, as the page spells it
        bool armed = false;
        std::optional<Price> bid;
        std::optional<Qty> bid_qty;
        std::optional<Price> ask;
        std::optional<Qty> ask_qty;
        std::optional<Price> mid;
        // Already formatted: the reference venue's prices are on its own
        // scale, not this one's, and nothing here should have to know which
        // venue is behind it. Empty when there is no usable reference.
        std::string reference_mid;
        // Why it is not quoting, empty while it is.
        std::string reason;
    };

    RecordLog(const RecordLog&) = delete;
    RecordLog& operator=(const RecordLog&) = delete;

    // Opens both files for `stamp` with their headers, so a directory that
    // cannot be written is found at startup. Returns why not, empty when
    // both opened.
    std::string open_now(const std::string& stamp);

    // One applied report. `strategy_id` is the strategy the order came from,
    // and is empty when it is not known: the ClOrdID does not carry it,
    // because the counter behind it is the daemon's and not a strategy's.
    void write(const AppliedReport& applied, const std::string& strategy_id);

    // One position's P&L at this instant. `mark` and the two halves that
    // depend on it are left empty when the book cannot say what the position
    // is worth, which is not the same answer as zero (GUI-Q15).
    void pnl_row(const Position& position, std::optional<Price> mark, const Pnl& pnl);

    // One quoting decision. Does nothing when the quotes file was not asked
    // for, so a caller need not check first.
    void quote_row(const Quote& quote);
    bool quotes_enabled() const { return quotes_ != nullptr; }

    const std::string& fills_path() const { return fills_file_.path(); }
    const std::string& orders_path() const { return orders_file_.path(); }

    const std::string& pnl_path() const { return pnl_file_.path(); }

private:
    DailyFile fills_file_;
    DailyFile orders_file_;
    DailyFile pnl_file_;
    DailyFile quotes_file_;
    CsvLog fills_;
    CsvLog orders_;
    CsvLog pnl_;
    // Held by pointer rather than by a flag, so a run that did not ask for
    // quotes has no file and no header rather than an empty one.
    std::unique_ptr<CsvLog> quotes_;
};

}  // namespace cpm::oms
