// Minimal FIX Market Data logon for Coinbase Prediction Markets (FIXT.1.1 / FIX 5.0 SP2).
//
// Credentials come from env vars:
//   CPM_API_KEY, CPM_PASSPHRASE, CPM_SECRET (base64 signing secret)
//   CPM_FIX_HOST, CPM_FIX_PORT, CPM_FIX_TLS (1 = wrap socket in TLS, default 1)
//
// Build: g++ -std=c++17 -O2 fix_md_login.cpp -o fix_md_login -lssl -lcrypto

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/ssl.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

const char SOH = '\x01';
const std::string TARGET_COMP_ID = "CPMMD";
const int HEARTBEAT_SECS = 30;

std::string env(const char* name, const char* fallback = nullptr) {
    const char* v = std::getenv(name);
    if (v) return v;
    if (fallback) return fallback;
    throw std::runtime_error(std::string("missing env var ") + name);
}

// ---------- crypto ----------

std::string base64_encode(const unsigned char* data, size_t len) {
    std::string out(4 * ((len + 2) / 3), '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), data, static_cast<int>(len));
    out.resize(n);
    return out;
}

std::string base64_decode(const std::string& in) {
    std::string out(3 * in.size() / 4, '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                            reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size()));
    if (n < 0) throw std::runtime_error("CPM_SECRET is not valid base64");
    // EVP_DecodeBlock counts '=' padding as zero bytes; trim them.
    size_t pad = 0;
    for (auto it = in.rbegin(); it != in.rend() && *it == '='; ++it) ++pad;
    out.resize(n - pad);
    return out;
}

// payload = SendingTime(ms, 21 chars) + ApiKey + TargetCompID + Password
std::string sign(const std::string& ms_time, const std::string& api_key,
                 const std::string& passphrase, const std::string& secret_b64) {
    std::string payload = ms_time + api_key + TARGET_COMP_ID + passphrase;
    std::string key = base64_decode(secret_b64);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest, &digest_len);
    return base64_encode(digest, digest_len);
}

// ---------- FIX encoding ----------

// Returns {header SendingTime with microseconds, millisecond form used for the signature}.
std::pair<std::string, std::string> sending_time() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d.%03lld", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms));
    std::string ms_ts = buf;  // yyyyMMdd-HH:mm:ss.SSS
    return {ms_ts + "000", ms_ts};
}

using Fields = std::vector<std::pair<int, std::string>>;

class FixSession {
public:
    FixSession(std::string api_key) : api_key_(std::move(api_key)) {}

    std::string build(const std::string& msg_type, const Fields& body_fields, std::string ts = "") {
        if (ts.empty()) ts = sending_time().first;
        Fields fields = {{35, msg_type}, {49, api_key_}, {56, TARGET_COMP_ID},
                         {34, std::to_string(++seq_num_)}, {52, ts}};
        fields.insert(fields.end(), body_fields.begin(), body_fields.end());

        std::string body;
        for (const auto& [tag, val] : fields) body += std::to_string(tag) + "=" + val + SOH;
        std::string msg = std::string("8=FIXT.1.1") + SOH + "9=" + std::to_string(body.size()) + SOH + body;

        unsigned sum = 0;
        for (unsigned char c : msg) sum += c;
        char cks[8];
        std::snprintf(cks, sizeof(cks), "%03u", sum % 256);
        return msg + "10=" + cks + SOH;
    }

private:
    std::string api_key_;
    int seq_num_ = 0;
};

std::map<std::string, std::string> parse(const std::string& raw) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t end = raw.find(SOH, pos);
        if (end == std::string::npos) end = raw.size();
        std::string field = raw.substr(pos, end - pos);
        size_t eq = field.find('=');
        if (eq != std::string::npos) out[field.substr(0, eq)] = field.substr(eq + 1);
        pos = end + 1;
    }
    return out;
}

// Masks the passphrase (554) and swaps SOH for '|' so messages are safe to print.
std::string printable(std::string s) {
    const std::string pwd = std::string(1, SOH) + "554=";
    size_t pos = s.find(pwd);
    if (pos != std::string::npos) {
        size_t start = pos + pwd.size();
        s.replace(start, s.find(SOH, start) - start, "***");
    }
    for (char& c : s)
        if (c == SOH) c = '|';
    return s;
}

// ---------- transport (plain TCP or TLS) ----------

class Connection {
public:
    Connection(const std::string& host, const std::string& port, bool use_tls) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
            throw std::runtime_error("cannot resolve " + host);
        for (addrinfo* p = res; p; p = p->ai_next) {
            fd_ = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd_ < 0) continue;
            if (connect(fd_, p->ai_addr, p->ai_addrlen) == 0) break;
            close(fd_);
            fd_ = -1;
        }
        freeaddrinfo(res);
        if (fd_ < 0) throw std::runtime_error("cannot connect to " + host + ":" + port);

        // recv() wakes up every second so the main loop can check whether a Heartbeat is due.
        timeval tv{1, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (use_tls) {
            ctx_ = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_default_verify_paths(ctx_);
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
            ssl_ = SSL_new(ctx_);
            SSL_set_tlsext_host_name(ssl_, host.c_str());
            SSL_set1_host(ssl_, host.c_str());
            SSL_set_fd(ssl_, fd_);
            if (SSL_connect(ssl_) != 1) {
                ERR_print_errors_fp(stderr);
                throw std::runtime_error("TLS handshake failed");
            }
        }
    }

    ~Connection() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (ctx_) SSL_CTX_free(ctx_);
        if (fd_ >= 0) close(fd_);
    }

    void send_all(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            int n = ssl_ ? SSL_write(ssl_, data.data() + sent, static_cast<int>(data.size() - sent))
                         : static_cast<int>(::send(fd_, data.data() + sent, data.size() - sent, 0));
            if (n <= 0) throw std::runtime_error("send failed");
            sent += n;
        }
    }

    // >0 bytes read, 0 = connection closed, -1 = timed out (idle).
    int recv_some(char* buf, int len) {
        if (ssl_) {
            int n = SSL_read(ssl_, buf, len);
            if (n > 0) return n;
            int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ || (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)))
                return -1;
            return 0;
        }
        ssize_t n = ::recv(fd_, buf, len, 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
        return n < 0 ? 0 : static_cast<int>(n);
    }

private:
    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

// Pops one complete FIX message (ending in "10=xxx<SOH>") off the front of buf, if present.
bool next_message(std::string& buf, std::string& msg) {
    size_t cks = buf.find(std::string(1, SOH) + "10=");
    if (cks == std::string::npos) return false;
    size_t end = buf.find(SOH, cks + 4);
    if (end == std::string::npos) return false;
    msg = buf.substr(0, end + 1);
    buf.erase(0, end + 1);
    return true;
}

}  // namespace

int main() {
    try {
        const std::string api_key = env("CPM_API_KEY");
        const std::string passphrase = env("CPM_PASSPHRASE");
        const std::string secret = env("CPM_SECRET");
        const std::string host = env("CPM_FIX_HOST");
        const std::string port = env("CPM_FIX_PORT");
        const bool use_tls = env("CPM_FIX_TLS", "1") == "1";

        Connection conn(host, port, use_tls);
        FixSession session(api_key);

        // Heartbeats are due HeartBtInt after the last message *we* sent; send a few seconds
        // early so the server never times us out and fires a TestRequest.
        auto last_sent = std::chrono::steady_clock::now();
        auto send = [&](const std::string& m) {
            std::cout << ">> " << printable(m) << std::endl;
            conn.send_all(m);
            last_sent = std::chrono::steady_clock::now();
        };

        auto [header_ts, ms_ts] = sending_time();
        std::string logon = session.build("A", {
            {98, "0"},                                  // EncryptMethod
            {108, std::to_string(HEARTBEAT_SECS)},      // HeartBtInt
            {141, "Y"},                                 // ResetSeqNumFlag
            {1137, "9"},                                // DefaultApplVerID = FIX50SP2
            {554, passphrase},                          // Password
            {58, sign(ms_ts, api_key, passphrase, secret)},  // Text = HMAC signature
        }, header_ts);
        send(logon);

        std::string buf, raw;
        char chunk[4096];
        while (true) {
            while (next_message(buf, raw)) {
                std::cout << "<< " << printable(raw) << std::endl;
                auto msg = parse(raw);
                const std::string& type = msg["35"];
                if (type == "A") {
                    std::cout << "Logon successful." << std::endl;
                } else if (type == "1") {  // TestRequest -> Heartbeat echoing TestReqID
                    send(session.build("0", {{112, msg["112"]}}));
                } else if (type == "5") {
                    std::cout << "Logged out: " << msg["58"] << std::endl;
                    return 0;
                }
            }
            if (std::chrono::steady_clock::now() - last_sent >= std::chrono::seconds(HEARTBEAT_SECS - 5))
                send(session.build("0", {}));

            int n = conn.recv_some(chunk, sizeof(chunk));
            if (n == 0) {
                std::cout << "Connection closed by server." << std::endl;
                return 0;
            }
            if (n > 0) buf.append(chunk, n);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
