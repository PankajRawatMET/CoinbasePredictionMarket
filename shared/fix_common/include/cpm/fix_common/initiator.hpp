#pragma once

#include <quickfix/Application.h>
#include <quickfix/Initiator.h>
#include <quickfix/Log.h>
#include <quickfix/MessageStore.h>
#include <quickfix/SessionSettings.h>

#include <memory>

namespace cpm::fix {

// Threaded socket initiator; TLS (QuickFIX built with OpenSSL) when tls is set.
std::unique_ptr<FIX::Initiator> make_initiator(FIX::Application& app, FIX::MessageStoreFactory& store,
                                               const FIX::SessionSettings& settings,
                                               FIX::LogFactory& log, bool tls);

}  // namespace cpm::fix
