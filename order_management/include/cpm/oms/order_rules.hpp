#pragma once

#include "cpm/common/config.hpp"
#include "cpm/oms/orders.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace cpm::oms {

// Client-side checks for the rules the gateway enforces on receipt, so a
// request that could only come back as BusinessMessageReject(j) is never sent.
// They return the reason as text, or nullopt when the request is sendable, and
// the wording names the field and the rule rather than guessing the gateway's
// own label.
//
// They are not a substitute for the gateway's validation: price bands, tick
// sizes, position and balance limits, instrument state and the registered SMP
// id are all server-side and come back as ExecutionReport(150=8) with an
// OrdRejReason(103).
std::optional<std::string> check_order(const NewOrder& order, const OeConfig& config);
std::optional<std::string> check_cancel(const CancelOrder& cancel);
std::optional<std::string> check_mass_cancel(const MassCancel& request);

// PartyID(448), the sub-account. The venue creates one lazily on first use, so
// any well-formed name works and an unknown one is not an error - but the
// accepted character set depends on the length, in bands:
//    1-18   any printable ASCII (0x20-0x7E)
//    19-20  alphanumerics and '-' only
//    21-31  lowercase hex (0-9, a-f) with no leading '0'
//    32-35  invalid
//    36     a lowercase canonical UUID (8-4-4-4-12)
//    >36    invalid
// Off-charset or over-length is answered with BusinessMessageReject(j)
// "invalid PartyID", so it is worth catching before the round trip.
std::optional<std::string> check_subaccount(const std::string& subaccount);

// The venue's limit on ClOrdID(11), in bytes.
inline constexpr std::size_t kClOrdIdMaxBytes = 16;

// ClOrdID: at most kClOrdIdMaxBytes of printable ASCII, and non-empty. The
// limit is in bytes, so multi-byte characters are refused rather than silently
// truncated.
std::optional<std::string> check_cl_ord_id(const std::string& cl_ord_id);

// A ClOrdID from a configured prefix plus a base36 counter (OE-Q9).
//
// Throws std::invalid_argument rather than returning something the gateway
// would reject: the 16-byte limit is the venue's, so an id that cannot fit is a
// configuration error, not a value to truncate. A base36 counter needs one
// character per 36x of range, so a prefix of 6 characters or fewer leaves room
// for 36^10 (about 3.6e15) orders, and the 10-character prefix run_prefix()
// builds leaves room for 36^6 (about 2.2e9) - 22 years at the rate three
// strategies quote.
std::string make_cl_ord_id(const std::string& prefix, std::uint64_t counter);

// Six base36 characters naming one run of the daemon: the Unix epoch second it
// started, wrapped into 36^6. Fixed width, so a prefix built from it is always
// the same length.
//
// This is what keeps ClOrdIDs unique across a restart (OE-Q9), and it replaced
// a counter persisted in the bookmark on every id allocated. That cost two
// fsyncs - about 3.3ms on a spinning disk - on the tick thread immediately
// before every send and every cancel, to buy a guarantee the clock already
// gives for nothing. Two runs cannot share a tag unless they start in the same
// second, and they cannot: one daemon holds the lock file at a time
// (cpm/common/lock_file.hpp), and a restart must log on and reconcile - seconds
// of it - before it may allocate anything.
//
// The wrap period is 36^6 seconds, about 69 years.
std::string run_tag(std::uint64_t epoch_seconds);
// This run's tag, from the system clock.
std::string run_tag();

// A ClOrdID prefix unique to one run: `role` + the tag + '-', so "mm-" becomes
// "mm-toqjx0-". Throws std::invalid_argument when that leaves no room for a
// counter inside the venue's 16 bytes.
std::string run_prefix(const std::string& role, const std::string& tag);

// Is ExecID `a` older than `b`? ExecIDs are decimal integers in one increasing
// space, carried as strings because they are identifiers and not arithmetic
// (OE-Q15), so they order by length first and then lexically. An empty id is
// older than anything, which is "we have processed nothing".
//
// There is one of these rather than one per caller because the bookmark and
// the gap check must agree about which of two ids is newer; when they did
// not, a bookmark that was newer than the exchange's retention window read as
// "behind" and asked for a replay that could not exist.
bool exec_id_before(const std::string& a, const std::string& b);

// Waits for a condition, polling. Used where a client has to wait for the
// session or the reconciler before it may act.
bool wait_for(const std::function<bool()>& ready, std::chrono::milliseconds timeout);

}  // namespace cpm::oms
