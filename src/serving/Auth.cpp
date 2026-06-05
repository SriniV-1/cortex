#include "serving/Auth.hpp"

#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

namespace cortex::serving {

// ── Base64url encode ─────────────────────────────────────────────────────

std::string base64url_encode(const unsigned char* data, size_t len) {
    // Standard base64 via OpenSSL, then convert to base64url.
    std::string out;
    out.resize(((len + 2) / 3) * 4 + 1);
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            data, static_cast<int>(len));
    out.resize(static_cast<size_t>(n));

    // base64 -> base64url: replace + with -, / with _, strip =
    for (auto& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    // Strip trailing padding
    while (!out.empty() && out.back() == '=') out.pop_back();

    return out;
}

std::string base64url_encode(const std::string& input) {
    return base64url_encode(reinterpret_cast<const unsigned char*>(input.data()),
                            input.size());
}

// ── Base64url decode ─────────────────────────────────────────────────────

std::string base64url_decode(const std::string& input) {
    // base64url -> standard base64
    std::string b64 = input;
    for (auto& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Re-add padding
    while (b64.size() % 4 != 0) b64 += '=';

    // Decode
    std::vector<unsigned char> buf(b64.size());
    int n = EVP_DecodeBlock(buf.data(),
                            reinterpret_cast<const unsigned char*>(b64.data()),
                            static_cast<int>(b64.size()));
    if (n < 0) return "";

    // EVP_DecodeBlock may over-count by up to 2 bytes due to padding.
    // Subtract bytes corresponding to padding '=' chars in the original.
    size_t padding = 0;
    for (auto it = b64.rbegin(); it != b64.rend() && *it == '='; ++it) ++padding;
    size_t actual = static_cast<size_t>(n) - padding;

    return std::string(reinterpret_cast<char*>(buf.data()), actual);
}

// ── HMAC-SHA256 signing ──────────────────────────────────────────────────

static std::string hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()),
         data.size(),
         digest, &digest_len);

    return std::string(reinterpret_cast<char*>(digest), digest_len);
}

// ── Minimal JSON helpers (avoid pulling nlohmann into Auth) ──────────────

// Produce a tiny JSON object with string and integer fields.
static std::string make_payload_json(const std::string& sub,
                                     const std::string& role,
                                     int64_t exp) {
    std::ostringstream oss;
    oss << R"({"sub":")" << sub
        << R"(","role":")" << role
        << R"(","exp":)" << exp << "}";
    return oss.str();
}

// Extract a JSON string value for a given key (minimal parser — no nesting).
static std::string json_extract_string(const std::string& json,
                                       const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Extract a JSON integer value for a given key.
static int64_t json_extract_int(const std::string& json,
                                const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    // Read digits (possibly with leading minus)
    std::string num;
    if (pos < json.size() && json[pos] == '-') { num += '-'; ++pos; }
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        num += json[pos++];
    }
    if (num.empty()) return 0;
    try { return std::stoll(num); } catch (...) { return 0; }
}

// ── JWT creation ─────────────────────────────────────────────────────────

std::string create_token(const std::string& sub,
                         const std::string& role,
                         const std::string& secret,
                         int expiry_sec) {
    // Header (always alg=HS256, typ=JWT)
    static const std::string header_json = R"({"alg":"HS256","typ":"JWT"})";
    std::string header_b64 = base64url_encode(header_json);

    // Payload
    auto now = std::chrono::system_clock::now();
    int64_t exp = std::chrono::duration_cast<std::chrono::seconds>(
                      now.time_since_epoch()).count() + expiry_sec;
    std::string payload_json = make_payload_json(sub, role, exp);
    std::string payload_b64 = base64url_encode(payload_json);

    // Signature
    std::string signing_input = header_b64 + "." + payload_b64;
    std::string sig_raw = hmac_sha256(secret, signing_input);
    std::string sig_b64 = base64url_encode(
        reinterpret_cast<const unsigned char*>(sig_raw.data()), sig_raw.size());

    return signing_input + "." + sig_b64;
}

// ── JWT validation ───────────────────────────────────────────────────────

std::optional<JWTClaims> validate_token(const std::string& token,
                                        const std::string& secret) {
    // Split into header.payload.signature
    auto dot1 = token.find('.');
    if (dot1 == std::string::npos) return std::nullopt;
    auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return std::nullopt;
    // No more dots expected
    if (token.find('.', dot2 + 1) != std::string::npos) return std::nullopt;

    std::string header_b64  = token.substr(0, dot1);
    std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string sig_b64     = token.substr(dot2 + 1);

    // Verify signature
    std::string signing_input = header_b64 + "." + payload_b64;
    std::string expected_sig = hmac_sha256(secret, signing_input);
    std::string expected_sig_b64 = base64url_encode(
        reinterpret_cast<const unsigned char*>(expected_sig.data()),
        expected_sig.size());

    if (sig_b64 != expected_sig_b64) return std::nullopt;

    // Decode payload
    std::string payload_json = base64url_decode(payload_b64);
    if (payload_json.empty()) return std::nullopt;

    JWTClaims claims;
    claims.sub  = json_extract_string(payload_json, "sub");
    claims.role = json_extract_string(payload_json, "role");
    claims.exp  = json_extract_int(payload_json, "exp");

    // Check expiration
    auto now = std::chrono::system_clock::now();
    int64_t now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                            now.time_since_epoch()).count();
    if (claims.exp <= now_epoch) return std::nullopt;

    return claims;
}

} // namespace cortex::serving
