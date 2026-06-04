// Unit tests for cursor-based pagination utilities.

#include "serving/Pagination.hpp"
#include "serving/Request.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace cortex::serving;

// ── Base64 encode/decode roundtrip ──────────────────────────────────────

TEST(PaginationTest, EncodeCursorDecodeRoundtrip) {
    json key = {{"game_id", "0022300123"}};
    std::string cursor = encode_cursor(key);
    EXPECT_FALSE(cursor.empty());

    json decoded = decode_cursor(cursor);
    EXPECT_FALSE(decoded.is_null());
    EXPECT_EQ(decoded["game_id"], "0022300123");
}

TEST(PaginationTest, EncodeCursorDecodeRoundtripComplex) {
    json key = {{"pts", 1500}, {"player_id", 203999}};
    std::string cursor = encode_cursor(key);

    json decoded = decode_cursor(cursor);
    EXPECT_FALSE(decoded.is_null());
    EXPECT_EQ(decoded["pts"], 1500);
    EXPECT_EQ(decoded["player_id"], 203999);
}

TEST(PaginationTest, EncodeCursorDecodeRoundtripRank) {
    json key = {{"rank", 10}};
    std::string cursor = encode_cursor(key);

    json decoded = decode_cursor(cursor);
    EXPECT_FALSE(decoded.is_null());
    EXPECT_EQ(decoded["rank"], 10);
}

// ── Decode invalid cursors ──────────────────────────────────────────────

TEST(PaginationTest, DecodeEmptyCursorReturnsNull) {
    json decoded = decode_cursor("");
    EXPECT_TRUE(decoded.is_null());
}

TEST(PaginationTest, DecodeGarbageCursorReturnsNull) {
    json decoded = decode_cursor("!!!not-valid-base64!!!");
    EXPECT_TRUE(decoded.is_null());
}

TEST(PaginationTest, DecodeValidBase64ButInvalidJsonReturnsNull) {
    // base64 encode a non-JSON string "hello world"
    std::string bad_cursor = detail::base64_encode("hello world");
    json decoded = decode_cursor(bad_cursor);
    EXPECT_TRUE(decoded.is_null());
}

// ── parse_pagination defaults ───────────────────────────────────────────

TEST(PaginationTest, ParsePaginationDefaults) {
    Request req;
    auto params = parse_pagination(req);

    EXPECT_EQ(params.limit, 50);
    EXPECT_TRUE(params.cursor.empty());
}

TEST(PaginationTest, ParsePaginationExplicitLimit) {
    Request req;
    req.query_params["limit"] = "25";

    auto params = parse_pagination(req);
    EXPECT_EQ(params.limit, 25);
}

TEST(PaginationTest, ParsePaginationLimitClampedToMax) {
    Request req;
    req.query_params["limit"] = "999";

    auto params = parse_pagination(req);
    EXPECT_EQ(params.limit, 200);
}

TEST(PaginationTest, ParsePaginationLimitClampedToMin) {
    Request req;
    req.query_params["limit"] = "0";

    auto params = parse_pagination(req);
    EXPECT_EQ(params.limit, 1);
}

TEST(PaginationTest, ParsePaginationNegativeLimitClampedToMin) {
    Request req;
    req.query_params["limit"] = "-10";

    auto params = parse_pagination(req);
    EXPECT_EQ(params.limit, 1);
}

TEST(PaginationTest, ParsePaginationInvalidLimitFallsBack) {
    Request req;
    req.query_params["limit"] = "abc";

    auto params = parse_pagination(req);
    EXPECT_EQ(params.limit, 50);  // query_int returns fallback of 50
}

TEST(PaginationTest, ParsePaginationWithCursor) {
    std::string cursor = encode_cursor(json{{"game_id", "0022300001"}});
    Request req;
    req.query_params["cursor"] = cursor;

    auto params = parse_pagination(req);
    EXPECT_EQ(params.cursor, cursor);
}

// ── paginated_response format ───────────────────────────────────────────

TEST(PaginationTest, PaginatedResponseWithMore) {
    json data = json::array({1, 2, 3});
    std::string cursor = "abc123";

    json resp = paginated_response(data, cursor, true);

    EXPECT_EQ(resp["data"], data);
    EXPECT_EQ(resp["next_cursor"], "abc123");
    EXPECT_EQ(resp["has_more"], true);
}

TEST(PaginationTest, PaginatedResponseNoMore) {
    json data = json::array({1, 2});

    json resp = paginated_response(data, "", false);

    EXPECT_EQ(resp["data"], data);
    EXPECT_TRUE(resp["next_cursor"].is_null());
    EXPECT_EQ(resp["has_more"], false);
}

TEST(PaginationTest, PaginatedResponseEmptyData) {
    json data = json::array();

    json resp = paginated_response(data, "", false);

    EXPECT_TRUE(resp["data"].is_array());
    EXPECT_EQ(resp["data"].size(), 0u);
    EXPECT_TRUE(resp["next_cursor"].is_null());
    EXPECT_EQ(resp["has_more"], false);
}

// ── Base64 specific edge cases ──────────────────────────────────────────

TEST(PaginationTest, Base64EncodeDecodeEmptyString) {
    std::string encoded = detail::base64_encode("");
    std::string decoded = detail::base64_decode(encoded);
    EXPECT_EQ(decoded, "");
}

TEST(PaginationTest, Base64PaddingCorrectness) {
    // 1-byte input needs == padding, 2-byte needs = padding
    std::string e1 = detail::base64_encode("a");
    EXPECT_EQ(e1.size() % 4, 0u);
    EXPECT_EQ(detail::base64_decode(e1), "a");

    std::string e2 = detail::base64_encode("ab");
    EXPECT_EQ(e2.size() % 4, 0u);
    EXPECT_EQ(detail::base64_decode(e2), "ab");

    std::string e3 = detail::base64_encode("abc");
    EXPECT_EQ(e3.size() % 4, 0u);
    EXPECT_EQ(detail::base64_decode(e3), "abc");
}
