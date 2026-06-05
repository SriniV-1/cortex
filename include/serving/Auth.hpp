#pragma once
// Auth — lightweight JWT authentication using HMAC-SHA256.
//
// Provides token creation, validation, and base64url helpers for
// the Cortex HTTP server's authentication middleware.

#include <cstdint>
#include <optional>
#include <string>

namespace cortex::serving {

// Roles used for RBAC.
// "viewer" — allowed GET endpoints only.
// "admin"  — allowed all endpoints including POST (ETL triggers, etc.).
struct JWTClaims {
    std::string sub;    // subject (user / service identity)
    std::string role;   // "viewer" or "admin"
    int64_t     exp;    // expiration (Unix epoch seconds)
};

// ── Base64url helpers (RFC 4648 §5, no padding) ──────────────────────────

std::string base64url_encode(const unsigned char* data, size_t len);
std::string base64url_encode(const std::string& input);
std::string base64url_decode(const std::string& input);

// ── JWT creation / validation ────────────────────────────────────────────

// Create a signed JWT with the given subject, role, and HMAC secret.
// The token expires after expiry_sec seconds from now.
std::string create_token(const std::string& sub,
                         const std::string& role,
                         const std::string& secret,
                         int expiry_sec = 3600);

// Validate a JWT string. Returns claims on success, nullopt on failure
// (bad format, invalid signature, expired, etc.).
std::optional<JWTClaims> validate_token(const std::string& token,
                                        const std::string& secret);

} // namespace cortex::serving
