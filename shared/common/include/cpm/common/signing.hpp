#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cpm {

struct SigningError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Standard padded Base64 (RFC 4648 section 4).
std::string base64_encode(std::span<const std::uint8_t> data);
// Strict: rejects bad length, characters outside the alphabet and misplaced padding.
std::vector<std::uint8_t> base64_decode(std::string_view text);

// base64(HMAC-SHA256(base64_decode(secret_b64), payload)), 44 characters.
std::string hmac_sha256_b64(std::string_view secret_b64, std::string_view payload);

// FIX Logon signature payload (spec FIX Order Entry "Signature payload", PDF p.50,
// applied to the MD gateway): SendingTime(ms, 21 chars) + ApiKey + TargetCompID
// + Password, concatenated with no separators.
std::string fix_logon_payload(std::string_view sending_time_ms, std::string_view api_key,
                              std::string_view target_comp_id, std::string_view passphrase);

std::string fix_logon_signature(std::string_view sending_time_ms, std::string_view api_key,
                                std::string_view target_comp_id, std::string_view passphrase,
                                std::string_view secret_b64);

}  // namespace cpm
