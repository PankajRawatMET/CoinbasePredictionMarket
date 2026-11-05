// The page cannot be driven here - there is no browser on this instance, and
// GUI-Q2 is the reason there never will be. So these tests hold the two things
// that would otherwise break it silently:
//
//   1. every field the page reads is in the JSON the daemon actually sends,
//      so renaming one in views.cpp fails a test rather than painting an
//      em dash where a price belongs; and
//   2. every path the page posts to is answered, and every claim the page
//      makes about being self-contained is true of the file that is embedded.
#include "cpm/gui/page.hpp"

#include "cpm/gui/json.hpp"
#include "cpm/gui/views.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace cpm::gui::test;
using cpm::gui::JsonValue;

std::string the_page() { return std::string(cpm::gui::page()); }

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A snapshot with one of everything, so no field is absent merely because
// nothing happened.
cpm::trader::Snapshot populated() {
    Daemon daemon;
    const auto created = daemon.api->create(cpm::strategy::StrategySpec{
        "CryptoMM", {{"symbol", kSym}, {"qty", "1"}}});
    EXPECT_TRUE(created.ok()) << created.error;
    EXPECT_TRUE(daemon.api->start(created.id).ok());
    EXPECT_TRUE(daemon.api->arm(created.id).ok());
    daemon.host->tick();  // one pass, so the strategy has legs to report

    PositionView position;
    position.symbol = kSym;
    position.net = 100;
    position.bought = 300;
    position.sold = 200;
    position.cash = -1'500'000;
    position.fees = 49'500;
    position.fills = 3;
    position.bought_value = 1'560'000;
    position.sold_value = 1'100'000;
    position.settlement_payout = 2'000'000;
    daemon.positions.push_back(position);

    cpm::trader::PnlView pnl;
    pnl.symbol = kSym;
    pnl.mark = 550;
    pnl.realized = 30'000;
    pnl.unrealized = 20'000;
    pnl.total = 50'000;
    daemon.pnl.push_back(pnl);
    return daemon.api->snapshot();
}

void expect_fields(const JsonValue& object, const std::vector<std::string>& names,
                   const std::string& where) {
    for (const auto& name : names) {
        EXPECT_NE(object.find(name), nullptr) << where << " has no \"" << name << "\"";
    }
}

TEST(Page, EveryFieldThePageReadsIsInTheSnapshot) {
    const auto json = cpm::gui::snapshot_json(populated());
    const auto parsed = cpm::gui::json_parse(json);
    ASSERT_TRUE(parsed.ok()) << parsed.error;

    expect_fields(parsed.value,
                  {"state", "allow_orders", "halt_reason", "recovery", "revision", "market_data",
                   "order_entry", "limits", "strategies", "books", "positions", "pnl",
                   "pnl_total"},
                  "the snapshot");
    expect_fields(*parsed.value.find("pnl_total"), {"realized", "unrealized", "total"},
                  "the P&L total");
    expect_fields(*parsed.value.find("market_data"), {"logged_on", "last_reason"}, "a session");
    expect_fields(*parsed.value.find("limits"),
                  {"max_quantity", "max_notional", "max_orders_per_sec"}, "the limits");

    const auto* strategies = parsed.value.find("strategies");
    ASSERT_EQ(strategies->items.size(), 1u);
    const auto& strategy = strategies->items[0];
    expect_fields(strategy,
                  {"id", "kind", "target", "state", "armed", "note", "reason", "symbols", "legs",
                   "counts"},
                  "a strategy");
    expect_fields(*strategy.find("counts"), {"placed", "cancelled", "fills"}, "the counts");
    ASSERT_FALSE(strategy.find("legs")->items.empty()) << "an armed strategy should be quoting";
    expect_fields(strategy.find("legs")->items[0],
                  {"side", "price", "quantity", "cancel_pending", "cl_ord_id"}, "a leg");

    const auto* books = parsed.value.find("books");
    ASSERT_FALSE(books->items.empty());
    expect_fields(books->items[0],
                  {"symbol", "valid", "status", "crossed_or_locked", "best_yes_bid",
                   "best_yes_bid_qty", "implied_yes_ask", "best_no_bid_qty", "mid", "last_trade",
                   "volume", "open_interest"},
                  "a book");

    const auto* positions = parsed.value.find("positions");
    ASSERT_FALSE(positions->items.empty());
    expect_fields(positions->items[0],
                  {"symbol", "net", "bought", "sold", "cash", "bought_value", "sold_value",
                   "fees", "fills", "settlement_payout", "status", "exit_price",
                   "exit_available", "exit_refusal"},
                  "a position");

    const auto* pnl = parsed.value.find("pnl");
    ASSERT_FALSE(pnl->items.empty());
    expect_fields(pnl->items[0], {"symbol", "mark", "realized", "unrealized", "total"}, "a P&L");
}

// The unrealized half of an unmarked position must reach the page as null, so
// the page can draw a dash: a zero there would read as a flat position when
// the truth is that there is no mid to mark against (GUI-Q15).
TEST(Page, AnUnmarkedPnlIsNullAndNotZero) {
    Daemon daemon;
    PositionView position;
    position.symbol = kSym;
    position.net = 100;
    daemon.positions.push_back(position);
    cpm::trader::PnlView pnl;
    pnl.symbol = kSym;
    pnl.realized = 30'000;  // mark, unrealized and total all unset
    daemon.pnl.push_back(pnl);

    const auto parsed = cpm::gui::json_parse(cpm::gui::snapshot_json(daemon.api->snapshot()));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const auto* rows = parsed.value.find("pnl");
    ASSERT_FALSE(rows->items.empty());
    const auto& row = rows->items[0];
    EXPECT_EQ(row.find("realized")->text, "0.03");
    for (const auto& field : {"mark", "unrealized", "total"}) {
        ASSERT_NE(row.find(field), nullptr) << field;
        EXPECT_EQ(row.find(field)->type, JsonValue::Type::Null) << field;
    }
    const auto* totals = parsed.value.find("pnl_total");
    EXPECT_EQ(totals->find("realized")->text, "0.03");
    EXPECT_EQ(totals->find("unrealized")->type, JsonValue::Type::Null);
    EXPECT_EQ(totals->find("total")->type, JsonValue::Type::Null);
}

TEST(Page, TheLadderArrivesWithBothSidesOnOnePriceAxis) {
    const auto json = cpm::gui::snapshot_json(populated());
    const auto parsed = cpm::gui::json_parse(json);
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const auto* books = parsed.value.find("books");
    ASSERT_FALSE(books->items.empty());
    const auto& book = books->items[0];

    expect_fields(book, {"bids", "asks", "spread"}, "a book");
    const auto& bids = book.find("bids")->items;
    const auto& asks = book.find("asks")->items;
    ASSERT_FALSE(bids.empty());
    ASSERT_FALSE(asks.empty());
    expect_fields(bids[0], {"price", "price_ticks", "qty", "venue_price"}, "a bid rung");

    // Best first on both sides, and both in YES terms: the ask rungs climb
    // away from the bids rather than being a second descending stack. The
    // price is the percentage chance the GUI shows; the ticks beside it are
    // the scaled integer everything below the GUI uses.
    EXPECT_EQ(bids[0].find("price")->text, "50");
    EXPECT_EQ(bids[0].find("price_ticks")->text, "500");
    for (std::size_t i = 1; i < bids.size(); ++i) {
        EXPECT_LT(std::stoll(bids[i].find("price_ticks")->text),
                  std::stoll(bids[i - 1].find("price_ticks")->text));
    }
    for (std::size_t i = 1; i < asks.size(); ++i) {
        EXPECT_GT(std::stoll(asks[i].find("price_ticks")->text),
                  std::stoll(asks[i - 1].find("price_ticks")->text));
    }
    EXPECT_LT(std::stoll(bids[0].find("price_ticks")->text),
              std::stoll(asks[0].find("price_ticks")->text));

    // The venue holds a NO bid on that side, and the rung says so, because an
    // operator comparing this screen with the venue's needs the number the
    // venue would show (Q9).
    EXPECT_EQ(asks[0].find("price")->text, "60");
    EXPECT_EQ(asks[0].find("venue_price")->text, "40");

    // A leg carries its price the same two ways, which is what lets the page
    // put an order on a rung without parsing a decimal.
    const auto* strategy = parsed.value.find("strategies")->items.data();
    const auto& legs = strategy->find("legs")->items;
    ASSERT_FALSE(legs.empty());
    expect_fields(legs[0], {"price", "price_ticks"}, "a leg");
}

TEST(Page, IsOnePagePerThingTheOperatorIsDoing) {
    const auto page = the_page();
    EXPECT_TRUE(contains(page, "id=\"nav\""));
    for (const std::string view : {"strategies", "ladders", "books", "positions"}) {
        EXPECT_TRUE(contains(page, "data-view=\"" + view + "\"")) << view << " has no section";
        EXPECT_TRUE(contains(page, "name: '" + view + "'")) << view << " is not in the nav";
    }
    // The view is in the address, so it survives a reload and can be sent to
    // someone - and stripping the token must not take it away with it.
    EXPECT_TRUE(contains(page, "window.location.pathname + window.location.hash"));
    EXPECT_TRUE(contains(page, "hashchange"));
    // Whatever page is open, the daemon's state, the halt banner and the kill
    // switch are above the nav and on all of them.
    const auto header = page.find("</header>");
    ASSERT_NE(header, std::string::npos);
    const auto top = page.substr(0, header);
    EXPECT_TRUE(contains(top, "id=\"stop-all\""));
    EXPECT_TRUE(contains(top, "id=\"pill-state\""));
    EXPECT_TRUE(contains(top, "id=\"nav\""));
    EXPECT_FALSE(contains(top, "data-view"));
}

TEST(Page, ShowsOurOwnOrdersOnTheLadder) {
    const auto page = the_page();
    EXPECT_TRUE(contains(page, "id=\"ladders\""));
    EXPECT_TRUE(contains(page, "renderLadders"));
    // Our rungs are a colour, not a column to go looking down.
    EXPECT_TRUE(contains(page, "mine-bid"));
    EXPECT_TRUE(contains(page, "mine-ask"));
    // Rungs are ordered and matched on ticks, never on the formatted price.
    EXPECT_TRUE(contains(page, "b.price_ticks - a.price_ticks"));
    EXPECT_FALSE(contains(page, "parseFloat"));
    // Sizes on a rung are listed, not added: no arithmetic on a quantity.
    EXPECT_TRUE(contains(page, "sizes.join('+')"));
}

TEST(Page, DrawsTheWholeGridAndAsksTheDaemonWhatItIs) {
    const auto page = the_page();
    // The axis is the instrument's tick grid and comes from the daemon once
    // per symbol. The page cannot generate it: stepping 0 to 1 in JavaScript
    // is decimal arithmetic on prices, which is what this page does not do.
    EXPECT_TRUE(contains(page, "/api/ladder?symbol="));
    EXPECT_TRUE(contains(page, "axes[symbol]"));
    EXPECT_FALSE(contains(page, "toFixed"));
    // Anything resting at a price the grid does not contain is still shown,
    // and marked, rather than silently dropped.
    EXPECT_TRUE(contains(page, "off_grid"));
    EXPECT_TRUE(contains(page, "not on the instrument"));
    // Built once and updated in place, because rebuilding hundreds of rows
    // twice a second would throw away wherever the operator had scrolled.
    EXPECT_TRUE(contains(page, "entry.signature !== signature"));
    EXPECT_TRUE(contains(page, "ladder-scroll"));
    // It centres itself once and then leaves the scroll alone.
    EXPECT_TRUE(contains(page, "entry.centred"));
    EXPECT_TRUE(contains(page, "looking at it"));
    // The axis is a percentage chance, and the column header is where it
    // says so: the ladder's own heading is the ticker and the centre button
    // and nothing else, so there is no line beside it to put it on.
    EXPECT_TRUE(contains(page, "chance %"));
    EXPECT_FALSE(contains(page, "rungs, 0-100%"));
    EXPECT_FALSE(contains(page, "entry.note"));
    EXPECT_TRUE(contains(page, "and nothing else"));
    // None of it is lost: a book that should not be quoted against, one that
    // is crossed, and a feed that is down all say so on Books.
    EXPECT_TRUE(contains(page, "'no valid book'"));
    EXPECT_TRUE(contains(page, "'crossed'"));
    EXPECT_TRUE(contains(page, "feed down: last known"));
    // 0 and 100 are rungs, but not prices an order can be placed at.
    EXPECT_TRUE(contains(page, "unquotable"));
    EXPECT_TRUE(contains(page, "a settlement value, not a price"));
}

TEST(Page, EveryFieldTheFormIsBuiltFromIsInTheSchema) {
    Daemon daemon;
    const auto parsed = cpm::gui::json_parse(cpm::gui::schema_json(daemon.api->kinds()));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const auto* kinds = parsed.value.find("kinds");
    ASSERT_NE(kinds, nullptr);
    ASSERT_FALSE(kinds->items.empty());
    expect_fields(kinds->items[0], {"name", "help", "params"}, "a kind");
    const auto* params = kinds->items[0].find("params");
    ASSERT_FALSE(params->items.empty());
    expect_fields(params->items[0],
                  {"name", "type", "default", "required", "help", "min", "max", "scale",
                   "choices"},
                  "a parameter");

    // The form switches on the type, so a type it does not know would render
    // as a text box with the wrong rules.
    for (const auto& param : params->items) {
        const auto type = param.find("type")->text;
        EXPECT_TRUE(type == "int" || type == "decimal" || type == "enum" || type == "text")
            << "the page has no input for a " << type;
    }
}

TEST(Page, ShowsTheControlsTheStageIsFor) {
    const auto page = the_page();
    // The G4 deliverable, item by item: a row per strategy, the blank row
    // built from the schema, the books, the positions, the halt banner,
    // Stop All.
    EXPECT_TRUE(contains(page, "id=\"strategy-rows\""));
    EXPECT_TRUE(contains(page, "id=\"draft-rows\""));
    EXPECT_TRUE(contains(page, "id=\"add-row\""));
    EXPECT_TRUE(contains(page, "id=\"log-lines\""));
    // The table it replaced is gone, not hidden (GUI-Q25).
    EXPECT_FALSE(contains(page, "id=\"strategies\""));
    EXPECT_TRUE(contains(page, "id=\"books\""));
    EXPECT_TRUE(contains(page, "id=\"positions\""));
    EXPECT_TRUE(contains(page, "id=\"halt\""));
    EXPECT_TRUE(contains(page, "id=\"stop-all\""));
    // A halt is terminal, and the page must not suggest a way out of it that
    // does not exist (GUI-Q4).
    EXPECT_TRUE(contains(page, "Nothing resumes in"));
    EXPECT_FALSE(contains(page, "Clear halt"));
    EXPECT_FALSE(contains(page, "Resume"));
}

TEST(Page, PutsEveryRowUnderOneHeaderOfColumns) {
    const auto page = the_page();
    // GUI-Q29: one header, and the three buttons under one Buttons column.
    EXPECT_TRUE(contains(page, "id=\"rows-head\""));
    EXPECT_TRUE(contains(page, "'Buttons|Kind|'"));
    // One grid for every row, not a grid per row: display:contents is what
    // makes a column a column across rows, and without it they drift.
    EXPECT_TRUE(contains(page, "display: contents"));
    EXPECT_TRUE(contains(page, "gridTemplateColumns"));
    // The column titles live in the header, once, not above every box.
    EXPECT_FALSE(contains(page, "el('label'"));
}

TEST(Page, GivesEveryRowTheSameButtons) {
    const auto page = the_page();
    // GUI-Q26, as GUI-Q34 and GUI-Q35 left it: Start and Stop, and those
    // two only, on a blank row and on a live one alike. Arm and Remove are
    // not buttons - Start arms, and the x is how a row goes.
    EXPECT_FALSE(contains(page, "actionButton('Create'"));
    EXPECT_TRUE(contains(page, "actionButton('Start'"));
    EXPECT_TRUE(contains(page, "actionButton('Stop'"));
    EXPECT_FALSE(contains(page, "actionButton('Apply'"));
    EXPECT_FALSE(contains(page, "actionButton('Revert'"));
    EXPECT_FALSE(contains(page, "actionButton('Arm'"));
    EXPECT_FALSE(contains(page, "actionButton('Disarm'"));
    EXPECT_FALSE(contains(page, "actionButton('Remove'"));
    // Stop withdraws the quotes and leaves the row, and the book stays:
    // the daemon owns what it watches, and a row's own book lives until the
    // x throws the row away (GUI-Q34). The feed verbs are not on the page
    // at all any more - they are an override, and they live on the console.
    EXPECT_FALSE(contains(page, "base + 'stop-feed'"));
    EXPECT_FALSE(contains(page, "base + 'start-feed'"));
    EXPECT_FALSE(contains(page, "actionButton('Start Feed'"));
    EXPECT_TRUE(contains(page, "'destroy'"));
    // The x needs a stopped row, not a feed that has gone.
    EXPECT_TRUE(contains(page, "actionButton('\\u00d7', 'drop', stopped"));
    // Start still asks before it sends an order (GUI-Q8), and it is the only
    // thing on the page that arms.
    EXPECT_TRUE(contains(page, "Start quoting"));
    EXPECT_TRUE(contains(page, "'arm'"));
    // A blank row is added, never conjured up.
    EXPECT_TRUE(contains(page, "Add row"));
}

TEST(Page, LocksTheParametersOnALiveRow) {
    const auto page = the_page();
    // GUI-Q30, as it now stands: with Apply gone there is nothing on the page
    // to send an edit with, so a live row's boxes are read-only rather than
    // accepting text they can never act on. The daemon still refuses an edit
    // to an armed strategy whatever any client believes.
    EXPECT_TRUE(contains(page, "var open = false;"));
    // What is typed outlives a redraw, and a redraw does not take the caret
    // out of a box being typed into.
    EXPECT_TRUE(contains(page, "var edits = {}"));
    EXPECT_TRUE(contains(page, "host.contains(focused)"));
    // The symbol is not among what an edit sends: moving a strategy is not a
    // parameter change.
    EXPECT_TRUE(contains(page, "if (name === 'symbol') return;"));
    // A locked box looks locked. Without this the only way to find out is
    // to click it and have nothing happen.
    EXPECT_TRUE(contains(page, ".field input:disabled"));
}

TEST(Page, LoadsNothingFromAnywhereElse) {
    const auto page = the_page();
    // The CSP the server sends is default-src 'none', so an external script,
    // stylesheet, font or image would simply not load - and the first anyone
    // would know of it is a blank panel at 02:00.
    EXPECT_FALSE(contains(page, "<script src"));
    EXPECT_FALSE(contains(page, "<link"));
    EXPECT_FALSE(contains(page, "https://"));
    EXPECT_FALSE(contains(page, "http://"));
    EXPECT_FALSE(contains(page, "//cdn"));
    // And nothing the daemon says is written as markup: a symbol or a halt
    // reason arrives from a FIX session.
    EXPECT_FALSE(contains(page, "innerHTML"));
    EXPECT_FALSE(contains(page, "document.write"));
}

TEST(Page, AsksBeforeItArmsAnything) {
    const auto page = the_page();
    // Arming is the one thing that reaches the venue, and Start is what does
    // it now (GUI-Q26), so Start goes through a dialog that names the
    // strategy, its symbol, its parameters and the limits it will trade
    // under - which window.confirm() cannot show.
    EXPECT_TRUE(contains(page, "title: 'Start quoting '"));
    EXPECT_TRUE(contains(page, "It will send live orders to the venue"));
    EXPECT_FALSE(contains(page, "window.confirm"));
    // Cancel holds the focus, so a stray Return does not arm anything.
    EXPECT_TRUE(contains(page, "cancel.focus()"));
    // Stop All asks too, and says what it is about to stop.
    EXPECT_TRUE(contains(page, "title: 'Stop every strategy?'"));
}

TEST(Page, KeepsTheKillSwitchReachable) {
    const auto page = the_page();
    // Three ways it could have become unreachable, each closed:
    // it is never disabled, it is never queued behind another request, and it
    // does not wait on a schema or a snapshot to work.
    EXPECT_FALSE(contains(page, "getElementById('stop-all').disabled"));
    EXPECT_TRUE(contains(page, "act('POST', '/api/stop-all', undefined, true)"));
    EXPECT_TRUE(contains(page, "if (busy && !always) return"));
    // And a request that never answers cannot leave the page stuck busy.
    EXPECT_TRUE(contains(page, "AbortController"));
    EXPECT_TRUE(contains(page, "REQUEST_TIMEOUT_MS"));
}

TEST(Page, SaysWhenItHasStoppedHearingFromTheDaemon) {
    const auto page = the_page();
    // A page showing the past as though it were now is the failure this whole
    // section exists to prevent.
    EXPECT_TRUE(contains(page, "GIVE_UP_AFTER"));
    EXPECT_TRUE(contains(page, "Nothing on this page is current"));
    EXPECT_TRUE(contains(page, "id=\"reconnect\""));
    // The heartbeat is what separates a quiet market from a dead tunnel.
    EXPECT_TRUE(contains(page, "'heartbeat'"));
    EXPECT_TRUE(contains(page, "silent "));
    // A missed revision re-snapshots rather than being merged over.
    EXPECT_TRUE(contains(page, "beat.revision !== snapshot.revision"));
    // And it stops looking live while it is not: a banner over numbers that
    // still read green is not an honest screen.
    EXPECT_TRUE(contains(page, "body.stale main"));
    EXPECT_TRUE(contains(page, "pill('pill-state', 'last known'"));
}

TEST(Page, KeepsTheTokenOutOfTheAddressBar) {
    const auto page = the_page();
    EXPECT_TRUE(contains(page, "history.replaceState"));
    EXPECT_TRUE(contains(page, "sessionStorage"));
    // The stream cannot send a header, so it is the one place the token stays
    // in a URL (GUI-Q3).
    EXPECT_TRUE(contains(page, "/api/events?t="));
    EXPECT_TRUE(contains(page, "'Authorization': 'Bearer '"));
}

// Everything the page posts to, actually posted. A path the page believes in
// and the server does not is a dead button.
class PagePaths : public ::testing::Test {
protected:
    void SetUp() override {
        Server::Options options;
        options.port = 0;
        server = std::make_unique<Server>(*daemon.api, options, daemon.logger);
        ASSERT_EQ(server->start(), "");
        ASSERT_TRUE(client.connect(server->port()));
    }
    void TearDown() override {
        client.disconnect();
        if (server) server->stop();
    }

    std::string send(const std::string& method, const std::string& target,
                     const std::string& body = {}) {
        std::string text = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n" +
                           "Authorization: Bearer " + server->token() + "\r\n";
        if (!body.empty()) text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        text += "\r\n" + body;
        EXPECT_TRUE(client.write(text));
        return client.response();
    }

    Daemon daemon;
    std::unique_ptr<Server> server;
    Client client;
};

TEST_F(PagePaths, EveryButtonOnThePageReachesSomething) {
    const auto page = the_page();
    const auto created =
        send("POST", "/api/strategies",
             R"({"kind":"CryptoMM","params":{"symbol":")" + kSym + R"(","qty":"1"}})");
    ASSERT_EQ(status_of(created), 200) << body_of(created);
    const auto id = cpm::gui::json_parse(body_of(created)).value.find("id")->text;

    // What the page calls, built as '/api/strategies/' + id + '/' + action.
    // Start arms, so arm is among them; Stop stops and then drops the feed;
    // Start Feed brings it back; and the x on a row with no feed destroys.
    // Disarm is not on the page at all.
    for (const std::string action : {"start", "arm", "stop", "destroy"}) {
        EXPECT_TRUE(contains(page, "'" + action + "'")) << action << " is not on the page";
    }
    EXPECT_FALSE(contains(page, "'disarm'")) << "nothing on the page disarms";
    // The feed verbs left the page with GUI-Q34: the daemon owns what it
    // watches, so there is no feed for a row to start or stop.
    EXPECT_FALSE(contains(page, "'start-feed'")) << "the page still drives the feed";
    EXPECT_FALSE(contains(page, "'stop-feed'")) << "the page still drives the feed";

    // Every one of them still answers: the page is not the only operator,
    // and the stdin console still has all of them.
    for (const std::string action : {"start", "arm", "disarm", "stop", "stop-feed",
                                     "start-feed", "destroy"}) {
        const auto response = send("POST", "/api/strategies/" + id + "/" + action);
        EXPECT_NE(status_of(response), 404) << action << " reached nothing";
        EXPECT_NE(status_of(response), 405) << action << " is the wrong method";
    }

    EXPECT_TRUE(contains(page, "/api/schema")) << "the form is built from the schema";
    EXPECT_EQ(status_of(send("GET", "/api/schema")), 200);
    // The page reads the schema once and then lives entirely on the stream:
    // it never polls /api/snapshot, so there is no second path by which what
    // it shows could disagree with what the daemon holds (GUI-Q14). The
    // endpoint stays for curl and for anything scripted.
    EXPECT_FALSE(contains(page, "/api/snapshot"));
    EXPECT_TRUE(contains(page, "EventSource"));
    EXPECT_EQ(status_of(send("GET", "/api/snapshot")), 200);
    EXPECT_TRUE(contains(page, "/api/stop-all"));
    EXPECT_EQ(status_of(send("POST", "/api/stop-all")), 200);

    // The Exit button on a position row (G13). The path it builds is answered
    // even with nothing held - a refusal, not a 404 or the wrong method.
    EXPECT_TRUE(contains(page, "'/api/positions/' + encodeURIComponent(position.symbol) + '/exit'"))
        << "the page does not post an exit anywhere";
    const auto exit_response = send("POST", "/api/positions/" + kSym + "/exit", R"({"qty":1})");
    EXPECT_NE(status_of(exit_response), 404) << "the exit path reached nothing";
    EXPECT_NE(status_of(exit_response), 405) << "the exit is the wrong method";
}

// The page decides nothing about whether a position may be exited: the daemon
// answers that in `exit_refusal`, and the button follows it. Two sources for
// one question is how a page comes to offer an order the daemon refuses - and
// on a closed window, that order is a reject, and a reject halts the run.
TEST(Page, TheExitButtonFollowsTheDaemonsRefusalAndNotItsOwnReading) {
    const auto page = the_page();
    EXPECT_TRUE(contains(page, "position.exit_refusal"));
    EXPECT_TRUE(contains(page, "actionButton('Exit'"));
    // It never reads a status to make up its own mind about tradability.
    EXPECT_FALSE(contains(page, "position.status === 'Open'"));

    Daemon daemon;
    PositionView position;
    position.symbol = kSym;
    position.net = 500;
    daemon.positions.push_back(position);

    auto parsed = cpm::gui::json_parse(cpm::gui::snapshot_json(daemon.api->snapshot()));
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const auto* open = &parsed.value.find("positions")->items[0];
    EXPECT_EQ(open->find("exit_refusal")->text, "");
    EXPECT_EQ(open->find("exit_price")->text, "50");       // percent, as the page shows prices
    EXPECT_EQ(open->find("exit_available")->text, "5");    // contracts resting at that price
    EXPECT_EQ(open->find("status")->text, "Open");

    // Halted, the same position carries the reason instead, which is what the
    // disabled button's tooltip shows.
    daemon.host->halt("market data: Gap on " + kSym);
    parsed = cpm::gui::json_parse(cpm::gui::snapshot_json(daemon.api->snapshot()));
    const auto* halted = &parsed.value.find("positions")->items[0];
    EXPECT_NE(halted->find("exit_refusal")->text.find("halted"), std::string::npos)
        << halted->find("exit_refusal")->text;
    EXPECT_EQ(halted->find("exit_price")->type, JsonValue::Type::Null);
}

TEST_F(PagePaths, TheServedPageIsTheFileInTheTree) {
    const auto response = send("GET", "/");
    ASSERT_EQ(status_of(response), 200);
    EXPECT_EQ(body_of(response), the_page());
    EXPECT_NE(response.find("Content-Security-Policy:"), std::string::npos);
}

}  // namespace
