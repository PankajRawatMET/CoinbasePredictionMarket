#include "cpm/oms/oe_client.hpp"

#include "cpm/fix_common/initiator.hpp"
#include "cpm/fix_common/raw_log.hpp"
#include "cpm/fix_common/session_settings.hpp"
#include "cpm/oms/order_rules.hpp"
#include "fix/oe_application.hpp"
#include "fix/oe_messages.hpp"

#include <quickfix/MessageStore.h>
#include <quickfix/Session.h>

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>

namespace cpm::oms {

struct OeClient::Impl {
    Impl(OeClientOptions opts, Logger& log)
        : options(std::move(opts)),
          logger(log),
          settings(cpm::fix::make_oe_initiator_settings(options.config, options.dict_dir,
                                                        options.reconnect_interval_secs)),
          app(options.config, logger),
          raw_log(logger, options.config.credentials.api_key) {}

    OeClientOptions options;
    Logger& logger;
    FIX::SessionSettings settings;
    fix::OeApplication app;
    // Nothing is persisted: order state is rebuilt with EventResendRequest(F3)
    // against a durable ExecID bookmark, not from a FIX message store.
    FIX::MemoryStoreFactory store;
    cpm::fix::RawLogFactory raw_log;
    std::mutex lifecycle_mutex;
    std::unique_ptr<FIX::Initiator> initiator;
    bool started = false;

    std::mutex limits_mutex;
    OrderLimits limits;
    std::function<bool()> submission_gate;
    std::deque<std::chrono::steady_clock::time_point> recent_orders;

    // Throws if the order breaks a configured limit. Rate limiting counts the
    // order as sent, so a refusal below does not consume a slot.
    void check_limits(const NewOrder& order);
    bool send_message(FIX::Message message);
};

void OeClient::Impl::check_limits(const NewOrder& order) {
    std::lock_guard lock(limits_mutex);
    if (!limits.allow_orders) {
        throw std::invalid_argument("orders are not enabled on this client (OrderLimits::allow_orders)");
    }
    if (limits.max_quantity > 0 && order.quantity > limits.max_quantity) {
        throw std::invalid_argument("OrderQty " + format_qty(order.quantity) + " exceeds the limit of " +
                                    format_qty(limits.max_quantity));
    }
    if (limits.max_notional > 0) {
        // A market order takes the worst admissible price, so bound it there.
        const Price price = order.type == OrderType::Limit
                                ? order.price
                                : (order.side == Side::Buy ? kPriceOne - 1 : 1);
        // quantity is hundredths and price thousandths, so the product is
        // 10^-5; amounts are 10^-6.
        const Amount notional = order.quantity * price * 10;
        if (notional > limits.max_notional) {
            throw std::invalid_argument("order notional " + format_amount(notional) +
                                        " exceeds the limit of " + format_amount(limits.max_notional));
        }
    }
    if (limits.max_orders_per_sec > 0) {
        const auto now = std::chrono::steady_clock::now();
        while (!recent_orders.empty() && now - recent_orders.front() >= std::chrono::seconds(1)) {
            recent_orders.pop_front();
        }
        if (static_cast<double>(recent_orders.size()) >= limits.max_orders_per_sec) {
            throw std::invalid_argument("order rate limit of " +
                                        std::to_string(limits.max_orders_per_sec) +
                                        "/s reached; retry shortly");
        }
        recent_orders.push_back(now);
    }
}

bool OeClient::Impl::send_message(FIX::Message message) {
    const auto session_id = app.session_id();
    if (!session_id) return false;
    try {
        return FIX::Session::sendToTarget(message, *session_id);
    } catch (const FIX::SessionNotFound&) {
        return false;
    }
}

OeCallbacks combine_oe(OeCallbacks first, OeCallbacks second) {
    const auto chain = [](auto a, auto b) {
        return [a = std::move(a), b = std::move(b)](const auto& event) {
            if (a) a(event);
            if (b) b(event);
        };
    };
    OeCallbacks out;
    out.report = chain(std::move(first.report), std::move(second.report));
    out.cancel_reject = chain(std::move(first.cancel_reject), std::move(second.cancel_reject));
    out.mass_cancel_report = chain(std::move(first.mass_cancel_report), std::move(second.mass_cancel_report));
    out.settlement = chain(std::move(first.settlement), std::move(second.settlement));
    out.last_exec_id = chain(std::move(first.last_exec_id), std::move(second.last_exec_id));
    out.resend_complete = chain(std::move(first.resend_complete), std::move(second.resend_complete));
    out.resend_reject = chain(std::move(first.resend_reject), std::move(second.resend_reject));
    out.business_reject = chain(std::move(first.business_reject), std::move(second.business_reject));
    out.decode_error = chain(std::move(first.decode_error), std::move(second.decode_error));
    return out;
}

OeClient::OeClient(OeClientOptions options, Logger& logger)
    : impl_(std::make_unique<Impl>(std::move(options), logger)) {}

OeClient::~OeClient() { stop(); }

void OeClient::on_session_event(SessionCallback callback) { impl_->app.set_callback(std::move(callback)); }

void OeClient::on_events(OeCallbacks callbacks) { impl_->app.set_event_callbacks(std::move(callbacks)); }

void OeClient::set_limits(OrderLimits limits) {
    std::lock_guard lock(impl_->limits_mutex);
    impl_->limits = limits;
}

void OeClient::set_submission_gate(std::function<bool()> gate) {
    std::lock_guard lock(impl_->limits_mutex);
    impl_->submission_gate = std::move(gate);
}

std::string OeClient::send(const NewOrder& order) {
    if (auto bad = check_order(order, impl_->options.config)) throw std::invalid_argument(*bad);
    {
        std::function<bool()> gate;
        {
            std::lock_guard lock(impl_->limits_mutex);
            gate = impl_->submission_gate;
        }
        // Reconciliation, or a halt: either way nothing new may be submitted.
        if (gate && !gate()) throw std::runtime_error("send: submission is not currently allowed");
    }
    impl_->check_limits(order);
    if (!logged_on()) throw std::runtime_error("send: not logged on");
    impl_->logger.info(std::string("sending order ") + order.cl_ord_id + " " + to_string(order.side) + " " +
                       format_qty(order.quantity) + " " + order.symbol + " @" +
                       (order.type == OrderType::Limit ? format_price(order.price) : std::string("market")) +
                       " " + to_string(order.time_in_force) +
                       (order.post_only == PostOnly::None ? "" : std::string(" ") + to_string(order.post_only)));
    if (!impl_->send_message(fix::new_order_single(order, impl_->options.config))) {
        throw std::runtime_error("send: order could not be sent");
    }
    return order.cl_ord_id;
}

bool OeClient::cancel(const CancelOrder& request) {
    if (auto bad = check_cancel(request)) throw std::invalid_argument(*bad);
    if (!logged_on()) return false;
    impl_->logger.info("sending cancel " + request.cl_ord_id + " for " +
                       (request.order_id.empty() ? request.orig_cl_ord_id : request.order_id));
    return impl_->send_message(fix::order_cancel_request(request, impl_->options.config));
}

bool OeClient::mass_cancel(const MassCancel& request) {
    if (auto bad = check_mass_cancel(request)) throw std::invalid_argument(*bad);
    if (!logged_on()) return false;
    impl_->logger.warn("sending mass cancel " + request.cl_ord_id);
    return impl_->send_message(fix::order_mass_cancel_request(request, impl_->options.config));
}

bool OeClient::request_last_exec_id() {
    if (!logged_on()) return false;
    return impl_->send_message(fix::last_exec_id_request());
}

bool OeClient::request_event_resend(const std::string& begin_exec_id, const std::string& end_exec_id) {
    if (begin_exec_id.empty()) throw std::invalid_argument("BeginExecId(22003) is required");
    if (!logged_on()) return false;
    return impl_->send_message(fix::event_resend_request(begin_exec_id, end_exec_id));
}

void OeClient::start() {
    std::lock_guard lock(impl_->lifecycle_mutex);
    if (impl_->started) return;
    const auto& ep = impl_->options.config.endpoint;
    impl_->logger.info("connecting to " + ep.host + ":" + std::to_string(ep.port) +
                       (ep.tls ? " (TLS)" : " (plain TCP)") + " as " +
                       redact_api_key(impl_->options.config.credentials.api_key));
    impl_->initiator = cpm::fix::make_initiator(impl_->app, impl_->store, impl_->settings,
                                                impl_->raw_log, ep.tls);
    impl_->initiator->start();
    impl_->started = true;
}

void OeClient::stop() {
    std::lock_guard lock(impl_->lifecycle_mutex);
    if (!impl_->started) return;
    impl_->started = false;
    impl_->initiator->stop();
    impl_->initiator.reset();
}

bool OeClient::wait_logged_on(std::chrono::milliseconds timeout) const {
    return impl_->app.wait_logged_on(timeout);
}

bool OeClient::logged_on() const { return impl_->app.logged_on(); }

std::string OeClient::last_logout_reason() const { return impl_->app.last_logout_reason(); }

bool OeClient::replaced() const { return impl_->app.replaced(); }

}  // namespace cpm::oms
