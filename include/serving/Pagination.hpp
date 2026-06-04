#pragma once
// Pagination — cursor-based pagination utilities for list endpoints.

#include "serving/Request.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>

namespace cortex::serving {

struct PaginationParams {
    int         limit  = 50;
    std::string cursor;       // raw base64 cursor string (empty if not provided)
};

// ── Base64 encode/decode (standard alphabet + padding) ──────────────────

namespace detail {

inline const char* b64_table() {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return t;
}

inline std::string base64_encode(const std::string& in) {
    const auto* tbl = b64_table();
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    unsigned val = 0;
    int bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(tbl[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(tbl[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

inline std::string base64_decode(const std::string& in) {
    static const int lookup[] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51
    };

    std::string out;
    unsigned val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (c >= 128 || lookup[c] == -1) return {};  // invalid
        val = (val << 6) + lookup[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

} // namespace detail

// ── Public API ──────────────────────────────────────────────────────────

inline PaginationParams parse_pagination(const Request& req) {
    PaginationParams p;
    p.limit = req.query_int("limit", 50);
    p.limit = std::max(1, std::min(p.limit, 200));
    p.cursor = req.query("cursor");
    return p;
}

inline std::string encode_cursor(const nlohmann::json& sort_key) {
    return detail::base64_encode(sort_key.dump());
}

inline nlohmann::json decode_cursor(const std::string& cursor) {
    if (cursor.empty()) return nullptr;
    try {
        std::string decoded = detail::base64_decode(cursor);
        if (decoded.empty()) return nullptr;
        return nlohmann::json::parse(decoded);
    } catch (...) {
        return nullptr;
    }
}

inline nlohmann::json paginated_response(const nlohmann::json& data,
                                          const std::string& next_cursor,
                                          bool has_more) {
    nlohmann::json j;
    j["data"] = data;
    j["next_cursor"] = has_more ? nlohmann::json(next_cursor) : nlohmann::json(nullptr);
    j["has_more"] = has_more;
    return j;
}

} // namespace cortex::serving
