#include "cpm/common/signing.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>

namespace cpm {
namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string base64_encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve(4 * ((data.size() + 2) / 3));
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (const std::size_t rest = data.size() - i; rest > 0) {
        std::uint32_t v = data[i] << 16;
        if (rest == 2) v |= data[i + 1] << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    if (text.empty() || text.size() % 4 != 0) {
        throw SigningError("base64: length must be a non-zero multiple of 4");
    }
    const std::size_t pad = text.ends_with("==") ? 2 : text.ends_with('=') ? 1 : 0;
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t acc = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        std::uint32_t v = 0;
        if (c == '=') {
            if (i < text.size() - pad) throw SigningError("base64: misplaced padding");
        } else {
            const auto pos = kAlphabet.find(c);
            if (pos == std::string_view::npos) throw SigningError("base64: invalid character");
            v = static_cast<std::uint32_t>(pos);
        }
        acc = (acc << 6) | v;
        if (i % 4 == 3) {
            out.push_back(static_cast<std::uint8_t>(acc >> 16));
            out.push_back(static_cast<std::uint8_t>(acc >> 8));
            out.push_back(static_cast<std::uint8_t>(acc));
            acc = 0;
        }
    }
    out.resize(out.size() - pad);
    return out;
}

std::string hmac_sha256_b64(std::string_view secret_b64, std::string_view payload) {
    const std::vector<std::uint8_t> key = base64_decode(secret_b64);
    std::array<std::uint8_t, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest.data(),
             &digest_len) == nullptr) {
        throw SigningError("HMAC-SHA256 failed");
    }
    return base64_encode(std::span(digest.data(), digest_len));
}

std::string fix_logon_payload(std::string_view sending_time_ms, std::string_view api_key,
                              std::string_view target_comp_id, std::string_view passphrase) {
    if (sending_time_ms.size() != 21) {
        throw SigningError("SendingTime for signing must be yyyyMMdd-HH:mm:ss.SSS (21 chars)");
    }
    std::string payload;
    payload.reserve(sending_time_ms.size() + api_key.size() + target_comp_id.size() +
                    passphrase.size());
    payload.append(sending_time_ms).append(api_key).append(target_comp_id).append(passphrase);
    return payload;
}

std::string fix_logon_signature(std::string_view sending_time_ms, std::string_view api_key,
                                std::string_view target_comp_id, std::string_view passphrase,
                                std::string_view secret_b64) {
    return hmac_sha256_b64(secret_b64,
                           fix_logon_payload(sending_time_ms, api_key, target_comp_id, passphrase));
}

}  // namespace cpm
