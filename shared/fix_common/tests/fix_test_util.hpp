#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace cpm::fix::test {

// Builds a wire FIX message from "tag=value" fields (header from 35 on, then body);
// BeginString, BodyLength and CheckSum are computed here.
inline std::string build_fix(const std::vector<std::pair<int, std::string>>& fields,
                             const std::string& begin_string = "FIXT.1.1") {
    std::string body;
    for (const auto& [tag, value] : fields) body += std::to_string(tag) + "=" + value + '\x01';
    std::string msg = "8=" + begin_string + '\x01' + "9=" + std::to_string(body.size()) + '\x01' + body;
    unsigned sum = 0;
    for (unsigned char c : msg) sum += c;
    char checksum[8];
    std::snprintf(checksum, sizeof(checksum), "%03u", sum % 256);
    return msg + "10=" + checksum + '\x01';
}

}  // namespace cpm::fix::test
