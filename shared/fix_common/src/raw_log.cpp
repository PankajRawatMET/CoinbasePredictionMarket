#include "cpm/fix_common/raw_log.hpp"

#include "cpm/common/config.hpp"

#include <array>

namespace cpm::fix {
namespace {

constexpr char kSoh = '\x01';
constexpr std::array<std::string_view, 3> kLogonSecretTags{"554", "58", "96"};

class RawLog : public FIX::Log {
public:
    RawLog(Logger& logger, std::string_view api_key) : logger_(logger), api_key_(api_key) {}

    void clear() override {}
    void backup() override {}
    void onIncoming(const std::string& msg) override { logger_.info("<< " + format_raw(msg, api_key_)); }
    void onOutgoing(const std::string& msg) override { logger_.info(">> " + format_raw(msg, api_key_)); }
    void onEvent(const std::string& text) override { logger_.info("-- " + redact_text(text, api_key_)); }

private:
    Logger& logger_;
    std::string api_key_;
};

bool is_logon(std::string_view message) {
    return message.find(std::string{kSoh} + "35=A" + kSoh) != std::string_view::npos;
}

}  // namespace

std::string redact_text(std::string_view text, std::string_view api_key) {
    std::string out(text);
    if (api_key.empty()) return out;
    const std::string replacement = redact_api_key(api_key);
    for (auto pos = out.find(api_key); pos != std::string::npos;
         pos = out.find(api_key, pos + replacement.size())) {
        out.replace(pos, api_key.size(), replacement);
    }
    return out;
}

std::string format_raw(std::string_view message, std::string_view api_key) {
    std::string out;
    out.reserve(message.size());
    const bool logon = is_logon(message);
    std::size_t start = 0;
    while (start < message.size()) {
        auto end = message.find(kSoh, start);
        if (end == std::string_view::npos) end = message.size();
        const std::string_view field = message.substr(start, end - start);
        const auto eq = field.find('=');
        const std::string_view tag = field.substr(0, eq);
        bool masked = false;
        if (logon && eq != std::string_view::npos) {
            for (auto secret : kLogonSecretTags) {
                if (tag == secret) masked = true;
            }
        }
        if (masked) {
            out.append(tag).append("=***");
        } else {
            out.append(field);
        }
        if (end < message.size()) out += '|';
        start = end + 1;
    }
    return redact_text(out, api_key);
}

RawLogFactory::RawLogFactory(Logger& logger, std::string api_key)
    : logger_(logger), api_key_(std::move(api_key)) {}

FIX::Log* RawLogFactory::create() { return new RawLog(logger_, api_key_); }

FIX::Log* RawLogFactory::create(const FIX::SessionID&) { return new RawLog(logger_, api_key_); }

void RawLogFactory::destroy(FIX::Log* log) { delete log; }

}  // namespace cpm::fix
