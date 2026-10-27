#pragma once

#include "cpm/md/events.hpp"

#include <quickfix/Message.h>

#include <string>
#include <string_view>
#include <vector>

namespace cpm::md::fix {

// Everything one message decoded into. A W or X can carry entries for several
// instruments and several channels, so each vector may hold more than one
// event, and `errors` can be non-empty beside decoded events.
struct DecodedBatch {
    std::vector<BookSnapshot> book_snapshots;
    std::vector<BookUpdate> book_updates;
    std::vector<Trade> trades;
    std::vector<Volume> volumes;
    std::vector<OpenInterest> open_interest;
    std::vector<InstrumentLifecycle> lifecycle;
    std::vector<MdReject> rejects;
    std::vector<DecodeError> errors;

    bool empty() const;
};

// Decodes MarketDataSnapshotFullRefresh(W), MarketDataIncrementalRefresh(X),
// SecurityStatus(f), MarketDataRequestReject(Y) and BusinessMessageReject(j)
// into the plain structs in cpm/md/events.hpp. Which channel an entry belongs
// to comes from MDEntryType(269), so no subscription state is needed; a W with
// no entries is an empty book snapshot (a volume/OI snapshot always carries
// MDEntrySize, spec §5.1).
//
// Never throws: a malformed or incomplete message becomes a DecodeError.
// `api_key` is redacted out of the raw text kept in those errors.
DecodedBatch decode_message(const FIX::Message& message, std::string_view api_key = {});

}  // namespace cpm::md::fix
