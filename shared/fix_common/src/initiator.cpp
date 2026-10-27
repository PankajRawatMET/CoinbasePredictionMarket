#include "cpm/fix_common/initiator.hpp"

#include <quickfix/ThreadedSSLSocketInitiator.h>
#include <quickfix/ThreadedSocketInitiator.h>

namespace cpm::fix {

std::unique_ptr<FIX::Initiator> make_initiator(FIX::Application& app, FIX::MessageStoreFactory& store,
                                               const FIX::SessionSettings& settings,
                                               FIX::LogFactory& log, bool tls) {
    if (tls) return std::make_unique<FIX::ThreadedSSLSocketInitiator>(app, store, settings, log);
    return std::make_unique<FIX::ThreadedSocketInitiator>(app, store, settings, log);
}

}  // namespace cpm::fix
