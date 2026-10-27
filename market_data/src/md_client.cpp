#include "cpm/md/md_client.hpp"

#include "cpm/fix_common/initiator.hpp"
#include "cpm/fix_common/raw_log.hpp"
#include "cpm/fix_common/session_settings.hpp"
#include "fix/md_application.hpp"

#include <quickfix/MessageStore.h>

#include <map>
#include <mutex>
#include <stdexcept>

namespace cpm::md {

struct MdClient::Impl {
    Impl(MdClientOptions opts, Logger& log)
        : options(std::move(opts)),
          logger(log),
          settings(cpm::fix::make_initiator_settings(options.config, options.dict_dir,
                                                     options.reconnect_interval_secs)),
          app(options.config.credentials, logger),
          raw_log(logger, options.config.credentials.api_key) {}

    MdClientOptions options;
    Logger& logger;
    FIX::SessionSettings settings;
    fix::MdApplication app;
    // Market data is not persisted or resent; recovery is resubscribe + snapshot.
    FIX::MemoryStoreFactory store;
    cpm::fix::RawLogFactory raw_log;
    // start() and stop() are serialised: a halt handler stops the feed from
    // the monitor thread while the owner may also stop it on shutdown, and
    // destroying the initiator while another thread is inside its stop()
    // pulls the QuickFIX session out from under its own socket thread.
    std::mutex lifecycle_mutex;
    std::unique_ptr<FIX::Initiator> initiator;
    bool started = false;

    std::mutex requests_mutex;
    int next_req = 1;
    std::map<std::string, ChannelRequest> active;  // MDReqID -> original request

    // Reserves a fresh MDReqID and records the request as active.
    std::string reserve(const ChannelRequest& request);
    // Sends the subscribe for a reserved MDReqID; on failure the reservation is dropped.
    void send_subscribe(const std::string& req_id, const ChannelRequest& request);
};

namespace {

const char* channel_name(Channel c) {
    switch (c) {
        case Channel::Book: return "book";
        case Channel::Trades: return "trades";
        case Channel::VolumeOi: return "volume-oi";
        case Channel::Lifecycle: return "lifecycle";
    }
    return "unknown";
}

// Client-side checks for the spec §4 / §4.3 rules, so we never send a request
// that can only be rejected.
void validate(const ChannelRequest& r) {
    if (r.channel == Channel::Book) {
        if (r.symbols.empty()) throw std::invalid_argument("book subscriptions must name instruments (281=9)");
        if (r.depth != 1 && r.depth != 10) throw std::invalid_argument("book depth must be 1 or 10 (281=5)");
    } else if (r.depth != 0) {
        throw std::invalid_argument("depth applies to book subscriptions only (281=5)");
    }
    for (const auto& s : r.symbols) {
        if (s.empty()) throw std::invalid_argument("empty symbol");
    }
}

}  // namespace

std::string MdClient::Impl::reserve(const ChannelRequest& request) {
    std::lock_guard lock(requests_mutex);
    // MDReqIDs only need to be unique among active requests on the session (281=1).
    auto req_id = std::string(channel_name(request.channel)) + "-" + std::to_string(next_req++);
    active[req_id] = request;
    return req_id;
}

void MdClient::Impl::send_subscribe(const std::string& req_id, const ChannelRequest& request) {
    if (app.send_market_data_request(req_id, request, true)) return;
    std::lock_guard lock(requests_mutex);
    active.erase(req_id);
    throw std::runtime_error("subscribe: request could not be sent");
}

MdCallbacks combine(MdCallbacks first, MdCallbacks second) {
    const auto chain = [](auto a, auto b) {
        return [a = std::move(a), b = std::move(b)](const auto& event) {
            if (a) a(event);
            if (b) b(event);
        };
    };
    MdCallbacks out;
    out.book_snapshot = chain(std::move(first.book_snapshot), std::move(second.book_snapshot));
    out.book_update = chain(std::move(first.book_update), std::move(second.book_update));
    out.trade = chain(std::move(first.trade), std::move(second.trade));
    out.volume = chain(std::move(first.volume), std::move(second.volume));
    out.open_interest = chain(std::move(first.open_interest), std::move(second.open_interest));
    out.lifecycle = chain(std::move(first.lifecycle), std::move(second.lifecycle));
    out.reject = chain(std::move(first.reject), std::move(second.reject));
    out.decode_error = chain(std::move(first.decode_error), std::move(second.decode_error));
    return out;
}

MdClient::MdClient(MdClientOptions options, Logger& logger)
    : impl_(std::make_unique<Impl>(std::move(options), logger)) {}

MdClient::~MdClient() { stop(); }

void MdClient::on_session_event(SessionCallback callback) { impl_->app.set_callback(std::move(callback)); }

void MdClient::on_market_data(MdCallbacks callbacks) { impl_->app.set_market_data_callbacks(std::move(callbacks)); }

void MdClient::start() {
    std::lock_guard lock(impl_->lifecycle_mutex);
    if (impl_->started) return;
    const auto& ep = impl_->options.config.endpoint;
    impl_->logger.info("connecting to " + ep.host + ":" + std::to_string(ep.port) +
                       (ep.tls ? " (TLS)" : " (plain TCP)"));
    impl_->initiator = cpm::fix::make_initiator(impl_->app, impl_->store, impl_->settings,
                                                impl_->raw_log, ep.tls);
    impl_->initiator->start();
    impl_->started = true;
}

// Safe to call from any thread, more than once, including from a FeedMonitor
// halt handler. Not from a QuickFIX callback: it waits for the logout.
void MdClient::stop() {
    std::lock_guard lock(impl_->lifecycle_mutex);
    if (!impl_->started) return;
    impl_->started = false;
    impl_->initiator->stop();
    impl_->initiator.reset();
}

bool MdClient::wait_logged_on(std::chrono::milliseconds timeout) const { return impl_->app.wait_logged_on(timeout); }

bool MdClient::logged_on() const { return impl_->app.logged_on(); }

std::string MdClient::last_logout_reason() const { return impl_->app.last_logout_reason(); }

std::string MdClient::subscribe(const ChannelRequest& request) {
    validate(request);
    if (!logged_on()) throw std::runtime_error("subscribe: not logged on");
    const auto req_id = impl_->reserve(request);
    impl_->send_subscribe(req_id, request);
    return req_id;
}

bool MdClient::unsubscribe(const std::string& md_req_id) {
    ChannelRequest request;
    {
        std::lock_guard lock(impl_->requests_mutex);
        const auto it = impl_->active.find(md_req_id);
        if (it == impl_->active.end()) return false;
        request = it->second;
        impl_->active.erase(it);
    }
    return logged_on() && impl_->app.send_market_data_request(md_req_id, request, false);
}

void MdClient::unsubscribe_all() {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(impl_->requests_mutex);
        for (const auto& [id, request] : impl_->active) ids.push_back(id);
    }
    for (const auto& id : ids) unsubscribe(id);
}

InstrumentList MdClient::list_instruments(std::chrono::milliseconds quiet, std::chrono::milliseconds timeout) {
    if (!logged_on()) throw std::runtime_error("list_instruments: not logged on");
    const ChannelRequest request{Channel::VolumeOi, {}, 0};
    const auto req_id = impl_->reserve(request);
    impl_->logger.info("requesting instrument list (" + req_id + ")");
    // Start collecting before sending, so a fast snapshot cannot be missed.
    impl_->app.begin_discovery(req_id);
    try {
        impl_->send_subscribe(req_id, request);
    } catch (...) {
        impl_->app.end_discovery();
        throw;
    }
    auto result = impl_->app.wait_discovery(quiet, timeout);
    impl_->app.end_discovery();
    if (result.reject.empty()) {
        unsubscribe(req_id);
    } else {
        std::lock_guard lock(impl_->requests_mutex);
        impl_->active.erase(req_id);
    }
    return result;
}

}  // namespace cpm::md
