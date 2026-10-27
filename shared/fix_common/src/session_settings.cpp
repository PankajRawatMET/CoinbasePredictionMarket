#include "cpm/fix_common/session_settings.hpp"

#include <quickfix/Dictionary.h>
#include <quickfix/SessionID.h>

namespace cpm::fix {

FIX::SessionSettings make_initiator_settings(const MdConfig& config, const std::string& dict_dir,
                                             int reconnect_interval_secs) {
    validate_endpoint(config.endpoint, kMdSandboxHost);

    FIX::Dictionary defaults;
    defaults.setString("ConnectionType", "initiator");
    defaults.setInt("ReconnectInterval", reconnect_interval_secs);
    defaults.setString("SocketNodelay", "Y");
    if (config.endpoint.tls) {
        defaults.setString("SSLProtocol", "TLSv1_2 +TLSv1_3");
        // Setting a CA file switches QuickFIX's client to SSL_VERIFY_PEER; the
        // patched initiator also checks the host name (cmake/patch_quickfix.cmake).
        defaults.setString("CertificationAuthoritiesFile", config.endpoint.ca_file);
    }

    FIX::Dictionary session;
    session.setString("BeginString", "FIXT.1.1");
    session.setString("DefaultApplVerID", "FIX.5.0SP2");
    session.setString("SenderCompID", config.credentials.api_key);
    session.setString("TargetCompID", config.target_comp_id);
    session.setString("SocketConnectHost", config.endpoint.host);
    session.setInt("SocketConnectPort", config.endpoint.port);
    session.setInt("HeartBtInt", config.heartbeat_secs);
    session.setString("StartTime", "00:00:00");
    session.setString("EndTime", "00:00:00");
    session.setString("UseDataDictionary", "Y");
    session.setString("TransportDataDictionary", dict_dir + "/" + kTransportDictionary);
    session.setString("AppDataDictionary", dict_dir + "/" + kAppDictionary);
    session.setString("ResetOnLogon", "Y");  // sends 141=Y
    session.setString("ResetOnLogout", "Y");
    session.setString("ResetOnDisconnect", "Y");
    session.setString("PersistMessages", "N");
    session.setInt("TimestampPrecision", 6);

    FIX::SessionSettings settings;
    settings.set(defaults);
    settings.set(FIX::SessionID("FIXT.1.1", config.credentials.api_key, config.target_comp_id), session);
    return settings;
}

FIX::SessionSettings make_oe_initiator_settings(const OeConfig& config, const std::string& dict_dir,
                                                int reconnect_interval_secs) {
    validate_endpoint(config.endpoint, kOeSandboxHost);

    FIX::Dictionary defaults;
    defaults.setString("ConnectionType", "initiator");
    defaults.setInt("ReconnectInterval", reconnect_interval_secs);
    defaults.setString("SocketNodelay", "Y");
    if (config.endpoint.tls) {
        defaults.setString("SSLProtocol", "TLSv1_2 +TLSv1_3");
        defaults.setString("CertificationAuthoritiesFile", config.endpoint.ca_file);
    }

    FIX::Dictionary session;
    session.setString("BeginString", "FIXT.1.1");
    session.setString("DefaultApplVerID", "FIX.5.0SP2");
    session.setString("SenderCompID", config.credentials.api_key);
    session.setString("TargetCompID", config.target_comp_id);
    session.setString("SocketConnectHost", config.endpoint.host);
    session.setInt("SocketConnectPort", config.endpoint.port);
    session.setInt("HeartBtInt", config.heartbeat_secs);
    session.setString("StartTime", "00:00:00");
    session.setString("EndTime", "00:00:00");
    session.setString("UseDataDictionary", "Y");
    session.setString("TransportDataDictionary", dict_dir + "/" + kOeTransportDictionary);
    session.setString("AppDataDictionary", dict_dir + "/" + kOeAppDictionary);
    session.setString("ResetOnLogon", "Y");  // sends 141=Y; sequence numbers do not persist
    session.setString("ResetOnLogout", "Y");
    session.setString("ResetOnDisconnect", "Y");
    // No message store: order state is rebuilt with EventResendRequest(F3)
    // against a durable ExecID bookmark, not from a FIX store (OE-Q7).
    session.setString("PersistMessages", "N");
    session.setInt("TimestampPrecision", 6);

    FIX::SessionSettings settings;
    settings.set(defaults);
    settings.set(FIX::SessionID("FIXT.1.1", config.credentials.api_key, config.target_comp_id), session);
    return settings;
}

FIX::SessionSettings make_oe_acceptor_settings(const std::string& client_api_key, std::uint16_t port,
                                               const std::string& dict_dir,
                                               const std::string& server_comp_id) {
    FIX::Dictionary defaults;
    defaults.setString("ConnectionType", "acceptor");
    defaults.setInt("SocketAcceptPort", port);
    defaults.setString("SocketReuseAddress", "Y");
    defaults.setString("SocketNodelay", "Y");

    FIX::Dictionary session;
    session.setString("BeginString", "FIXT.1.1");
    session.setString("DefaultApplVerID", "FIX.5.0SP2");
    session.setString("SenderCompID", server_comp_id);
    session.setString("TargetCompID", client_api_key);
    session.setString("StartTime", "00:00:00");
    session.setString("EndTime", "00:00:00");
    session.setString("UseDataDictionary", "Y");
    session.setString("TransportDataDictionary", dict_dir + "/" + kOeTransportDictionary);
    session.setString("AppDataDictionary", dict_dir + "/" + kOeAppDictionary);
    session.setString("ResetOnLogon", "Y");
    session.setString("ResetOnLogout", "Y");
    session.setString("ResetOnDisconnect", "Y");
    session.setString("PersistMessages", "N");
    session.setInt("TimestampPrecision", 6);

    FIX::SessionSettings settings;
    settings.set(defaults);
    settings.set(FIX::SessionID("FIXT.1.1", server_comp_id, client_api_key), session);
    return settings;
}

FIX::SessionSettings make_acceptor_settings(const std::string& client_api_key, std::uint16_t port,
                                            const std::string& dict_dir,
                                            const std::string& server_comp_id) {
    FIX::Dictionary defaults;
    defaults.setString("ConnectionType", "acceptor");
    defaults.setInt("SocketAcceptPort", port);
    defaults.setString("SocketReuseAddress", "Y");
    defaults.setString("SocketNodelay", "Y");

    FIX::Dictionary session;
    session.setString("BeginString", "FIXT.1.1");
    session.setString("DefaultApplVerID", "FIX.5.0SP2");
    session.setString("SenderCompID", server_comp_id);
    session.setString("TargetCompID", client_api_key);
    session.setString("StartTime", "00:00:00");
    session.setString("EndTime", "00:00:00");
    session.setString("UseDataDictionary", "Y");
    session.setString("TransportDataDictionary", dict_dir + "/" + kTransportDictionary);
    session.setString("AppDataDictionary", dict_dir + "/" + kAppDictionary);
    session.setString("ResetOnLogon", "Y");
    session.setString("ResetOnLogout", "Y");
    session.setString("ResetOnDisconnect", "Y");
    session.setString("PersistMessages", "N");
    session.setInt("TimestampPrecision", 6);

    FIX::SessionSettings settings;
    settings.set(defaults);
    settings.set(FIX::SessionID("FIXT.1.1", server_comp_id, client_api_key), session);
    return settings;
}

}  // namespace cpm::fix
