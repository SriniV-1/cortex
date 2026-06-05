// Unit tests for JWT authentication (Auth.hpp / Auth.cpp).

#include "serving/Auth.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace cortex::serving;

static const std::string kSecret = "test-secret-key-for-unit-tests";

// ── Base64url round-trip ─────────────────────────────────────────────────

TEST(AuthTest, Base64urlRoundTrip) {
    std::string original = "Hello, JWT world! Special chars: +/=";
    std::string encoded = base64url_encode(original);

    // base64url must not contain +, /, or =
    EXPECT_EQ(encoded.find('+'), std::string::npos);
    EXPECT_EQ(encoded.find('/'), std::string::npos);
    EXPECT_EQ(encoded.find('='), std::string::npos);

    std::string decoded = base64url_decode(encoded);
    EXPECT_EQ(decoded, original);
}

TEST(AuthTest, Base64urlEmptyString) {
    std::string encoded = base64url_encode("");
    std::string decoded = base64url_decode(encoded);
    EXPECT_EQ(decoded, "");
}

TEST(AuthTest, Base64urlBinaryData) {
    // Test with bytes that exercise all base64 alphabet positions
    std::string binary;
    for (int i = 0; i < 256; ++i) binary += static_cast<char>(i);
    std::string encoded = base64url_encode(binary);
    std::string decoded = base64url_decode(encoded);
    EXPECT_EQ(decoded, binary);
}

// ── Token creation and validation round-trip ─────────────────────────────

TEST(AuthTest, CreateAndValidateToken) {
    std::string token = create_token("user42", "admin", kSecret, 3600);

    // Token has three dot-separated parts
    auto dot1 = token.find('.');
    ASSERT_NE(dot1, std::string::npos);
    auto dot2 = token.find('.', dot1 + 1);
    ASSERT_NE(dot2, std::string::npos);
    EXPECT_EQ(token.find('.', dot2 + 1), std::string::npos);

    auto claims = validate_token(token, kSecret);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->sub, "user42");
    EXPECT_EQ(claims->role, "admin");
    EXPECT_GT(claims->exp, 0);
}

TEST(AuthTest, ViewerRoleRoundTrip) {
    std::string token = create_token("readonly", "viewer", kSecret, 3600);
    auto claims = validate_token(token, kSecret);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->sub, "readonly");
    EXPECT_EQ(claims->role, "viewer");
}

// ── Expired token rejected ───────────────────────────────────────────────

TEST(AuthTest, ExpiredTokenRejected) {
    // Create a token that expired 10 seconds ago
    std::string token = create_token("user", "admin", kSecret, -10);
    auto claims = validate_token(token, kSecret);
    EXPECT_FALSE(claims.has_value());
}

// ── Invalid signature rejected ───────────────────────────────────────────

TEST(AuthTest, InvalidSignatureRejected) {
    std::string token = create_token("user", "admin", kSecret, 3600);

    // Validate with wrong secret
    auto claims = validate_token(token, "wrong-secret");
    EXPECT_FALSE(claims.has_value());
}

TEST(AuthTest, TamperedPayloadRejected) {
    std::string token = create_token("user", "admin", kSecret, 3600);

    // Tamper with the payload section (flip a character)
    auto dot1 = token.find('.');
    auto dot2 = token.find('.', dot1 + 1);
    size_t mid = dot1 + (dot2 - dot1) / 2;
    token[mid] = (token[mid] == 'A') ? 'B' : 'A';

    auto claims = validate_token(token, kSecret);
    EXPECT_FALSE(claims.has_value());
}

// ── Malformed tokens ─────────────────────────────────────────────────────

TEST(AuthTest, EmptyTokenRejected) {
    EXPECT_FALSE(validate_token("", kSecret).has_value());
}

TEST(AuthTest, SinglePartRejected) {
    EXPECT_FALSE(validate_token("abc", kSecret).has_value());
}

TEST(AuthTest, TwoPartsRejected) {
    EXPECT_FALSE(validate_token("abc.def", kSecret).has_value());
}

TEST(AuthTest, FourPartsRejected) {
    EXPECT_FALSE(validate_token("a.b.c.d", kSecret).has_value());
}

// ── Role extraction works correctly ──────────────────────────────────────

TEST(AuthTest, RoleExtractionAdmin) {
    auto token = create_token("svc", "admin", kSecret);
    auto claims = validate_token(token, kSecret);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->role, "admin");
}

TEST(AuthTest, RoleExtractionViewer) {
    auto token = create_token("svc", "viewer", kSecret);
    auto claims = validate_token(token, kSecret);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->role, "viewer");
}

// ── Expiration is set correctly ──────────────────────────────────────────

TEST(AuthTest, ExpirationSetCorrectly) {
    auto before = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto token = create_token("u", "admin", kSecret, 7200);
    auto claims = validate_token(token, kSecret);
    ASSERT_TRUE(claims.has_value());

    auto after = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // exp should be within [before + 7200, after + 7200]
    EXPECT_GE(claims->exp, before + 7200);
    EXPECT_LE(claims->exp, after + 7200);
}
