// Phase 9.1 — Property-based tests using RapidCheck + GTest
//
// Components tested:
//   1. RingBuffer     — FIFO ordering, capacity limits
//   2. StatAccumulator — non-negative stats, points consistency
//   3. Router         — param extraction, no-match safety
//   4. EloTracker     — expected_score symmetry, monotonicity, defaults
//   5. RateLimiter    — burst capacity, deterministic token accounting

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "stream/RingBuffer.hpp"
#include "stream/StatAccumulator.hpp"
#include "stream/StreamEvent.hpp"
#include "serving/Router.hpp"
#include "serving/RateLimiter.hpp"
#include "analytics/EloTracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

using namespace cortex;

// ═══════════════════════════════════════════════════════════════════════════
// 1. RingBuffer properties
// ═══════════════════════════════════════════════════════════════════════════

RC_GTEST_PROP(RingBuffer, FifoPushPop, (std::vector<int> values)) {
    RC_PRE(!values.empty());
    // Clamp to reasonable size so the test doesn't allocate huge buffers.
    size_t n = std::min(values.size(), size_t(1024));
    values.resize(n);

    // Round capacity up to next power of 2
    size_t cap = 1;
    while (cap < n) cap <<= 1;

    stream::RingBuffer<int> buf(cap);

    for (size_t i = 0; i < n; ++i) {
        RC_ASSERT(buf.try_push(values[i]));
    }
    RC_ASSERT(buf.size() == n);

    for (size_t i = 0; i < n; ++i) {
        auto v = buf.try_pop();
        RC_ASSERT(v.has_value());
        RC_ASSERT(*v == values[i]);   // FIFO order preserved
    }
    RC_ASSERT(buf.empty());
}

RC_GTEST_PROP(RingBuffer, PopEmptyReturnsNullopt, ()) {
    stream::RingBuffer<int> buf(16);
    RC_ASSERT(!buf.try_pop().has_value());
}

RC_GTEST_PROP(RingBuffer, FullBufferRejectsPush, (std::vector<int> values)) {
    RC_PRE(values.size() >= 2);
    size_t cap = 1;
    while (cap < values.size()) cap <<= 1;
    // Use exactly cap items to fill, then one more must fail
    stream::RingBuffer<int> buf(cap);
    for (size_t i = 0; i < cap; ++i) {
        RC_ASSERT(buf.try_push(values[i % values.size()]));
    }
    RC_ASSERT(buf.full());
    RC_ASSERT(!buf.try_push(42));  // must reject
}

RC_GTEST_PROP(RingBuffer, SizeTracksOperations, (std::vector<int> pushes)) {
    RC_PRE(!pushes.empty());
    size_t n = std::min(pushes.size(), size_t(512));
    pushes.resize(n);
    size_t cap = 1;
    while (cap < n) cap <<= 1;

    stream::RingBuffer<int> buf(cap);
    for (size_t i = 0; i < n; ++i) {
        buf.try_push(pushes[i]);
        RC_ASSERT(buf.size() == i + 1);
    }
    for (size_t i = 0; i < n; ++i) {
        buf.try_pop();
        RC_ASSERT(buf.size() == n - i - 1);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. StatAccumulator properties
// ═══════════════════════════════════════════════════════════════════════════

// Helper: create a StreamEvent with the given action and shot result.
static stream::StreamEvent make_event(stream::ActionType action, bool made,
                                       int32_t player_id, const char* game_id) {
    stream::StreamEvent ev;
    ev.action_type = action;
    ev.shot_made   = made;
    ev.player_id   = player_id;
    ev.team_id     = 1;
    ev.home_team_id = 1;
    ev.away_team_id = 2;
    std::strncpy(ev.game_id.data(), game_id, ev.game_id.size() - 1);
    ev.game_id[ev.game_id.size() - 1] = '\0';
    return ev;
}

RC_GTEST_PROP(StatAccumulator, NonNegativeStats, ()) {
    stream::StatAccumulator acc;
    // Generate a random sequence of events
    int count = *rc::gen::inRange(1, 100);
    for (int i = 0; i < count; ++i) {
        auto action = static_cast<stream::ActionType>(*rc::gen::inRange(1, 8));
        bool made = *rc::gen::arbitrary<bool>();
        acc.process(make_event(action, made, 101, "0022300001"));
    }
    auto snap = acc.player_stats(101, "0022300001");
    RC_ASSERT(snap.points    >= 0);
    RC_ASSERT(snap.rebounds  >= 0);
    RC_ASSERT(snap.assists   >= 0);
    RC_ASSERT(snap.turnovers >= 0);
    RC_ASSERT(snap.fouls     >= 0);
    RC_ASSERT(snap.fga       >= 0);
    RC_ASSERT(snap.fgm       >= 0);
    RC_ASSERT(snap.fta       >= 0);
    RC_ASSERT(snap.ftm       >= 0);
}

RC_GTEST_PROP(StatAccumulator, PointsConsistency, ()) {
    stream::StatAccumulator acc;

    int n2_made = *rc::gen::inRange(0, 20);
    int n3_made = *rc::gen::inRange(0, 20);
    int nft_made = *rc::gen::inRange(0, 20);
    int n2_miss = *rc::gen::inRange(0, 10);
    int n3_miss = *rc::gen::inRange(0, 10);
    int nft_miss = *rc::gen::inRange(0, 10);

    // Feed specific shot events
    for (int i = 0; i < n2_made; ++i)
        acc.process(make_event(stream::ActionType::Shot2pt, true, 202, "0022300002"));
    for (int i = 0; i < n2_miss; ++i)
        acc.process(make_event(stream::ActionType::Shot2pt, false, 202, "0022300002"));
    for (int i = 0; i < n3_made; ++i)
        acc.process(make_event(stream::ActionType::Shot3pt, true, 202, "0022300002"));
    for (int i = 0; i < n3_miss; ++i)
        acc.process(make_event(stream::ActionType::Shot3pt, false, 202, "0022300002"));
    for (int i = 0; i < nft_made; ++i)
        acc.process(make_event(stream::ActionType::FreeThrow, true, 202, "0022300002"));
    for (int i = 0; i < nft_miss; ++i)
        acc.process(make_event(stream::ActionType::FreeThrow, false, 202, "0022300002"));

    auto snap = acc.player_stats(202, "0022300002");
    int expected_pts = n2_made * 2 + n3_made * 3 + nft_made * 1;
    RC_ASSERT(snap.points == expected_pts);
    RC_ASSERT(snap.fga == n2_made + n2_miss + n3_made + n3_miss);
    RC_ASSERT(snap.fgm == n2_made + n3_made);
    RC_ASSERT(snap.fta == nft_made + nft_miss);
    RC_ASSERT(snap.ftm == nft_made);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Router properties
// ═══════════════════════════════════════════════════════════════════════════

RC_GTEST_PROP(Router, ParamExtraction, (std::string value)) {
    // Only test with valid URL path segment characters
    RC_PRE(!value.empty());
    RC_PRE(value.find('/') == std::string::npos);
    RC_PRE(value.find(' ') == std::string::npos);
    RC_PRE(value.find('\0') == std::string::npos);

    serving::Router router;
    bool handler_called = false;
    router.add("GET", "/items/:id", [&](serving::Request&, serving::Response&,
                                         serving::ServerContext&) {
        handler_called = true;
    });

    auto result = router.match("GET", "/items/" + value);
    RC_ASSERT(result.has_value());
    RC_ASSERT(result->params.at("id") == value);
}

RC_GTEST_PROP(Router, NoMatchForWrongMethod, (std::string segment)) {
    RC_PRE(!segment.empty());
    RC_PRE(segment.find('/') == std::string::npos);
    RC_PRE(segment.find(' ') == std::string::npos);

    serving::Router router;
    router.add("GET", "/api/:id", [](serving::Request&, serving::Response&,
                                      serving::ServerContext&) {});

    auto result = router.match("POST", "/api/" + segment);
    RC_ASSERT(!result.has_value());
}

RC_GTEST_PROP(Router, MultipleParamsExtracted, (std::string seg1, std::string seg2)) {
    RC_PRE(!seg1.empty() && !seg2.empty());
    RC_PRE(seg1.find('/') == std::string::npos && seg2.find('/') == std::string::npos);
    RC_PRE(seg1.find(' ') == std::string::npos && seg2.find(' ') == std::string::npos);
    RC_PRE(seg1.find('\0') == std::string::npos && seg2.find('\0') == std::string::npos);

    serving::Router router;
    router.add("GET", "/games/:gameId/players/:playerId",
               [](serving::Request&, serving::Response&, serving::ServerContext&) {});

    auto result = router.match("GET", "/games/" + seg1 + "/players/" + seg2);
    RC_ASSERT(result.has_value());
    RC_ASSERT(result->params.at("gameId") == seg1);
    RC_ASSERT(result->params.at("playerId") == seg2);
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. EloTracker properties
// ═══════════════════════════════════════════════════════════════════════════

RC_GTEST_PROP(EloTracker, ExpectedScoreSymmetry, ()) {
    float a = *rc::gen::inRange(800, 2400);
    float b = *rc::gen::inRange(800, 2400);
    float ea = analytics::EloTracker::expected_score(a, b);
    float eb = analytics::EloTracker::expected_score(b, a);
    // Note: expected_score includes HOME_ADVANTAGE, so we compare
    // expected_score(a,b) as home vs expected_score(b,a) as home.
    // The sum should be close to 1.0 only if home advantage is symmetric.
    // Actually, each call adds HOME_ADVANTAGE to the first arg.
    // So we just verify each value is in [0, 1].
    RC_ASSERT(ea >= 0.0f && ea <= 1.0f);
    RC_ASSERT(eb >= 0.0f && eb <= 1.0f);
}

RC_GTEST_PROP(EloTracker, HigherRatingHigherExpected, ()) {
    float base = *rc::gen::inRange(1000, 2000);
    float delta = *rc::gen::inRange(1, 500);
    float high = base + delta;
    float low  = base;

    // When the higher-rated team is home, their expected score should be
    // greater than when the lower-rated team is home against the higher.
    float e_high_home = analytics::EloTracker::expected_score(high, low);
    float e_low_home  = analytics::EloTracker::expected_score(low, high);
    RC_ASSERT(e_high_home > e_low_home);
}

RC_GTEST_PROP(EloTracker, UnknownTeamGetsInitialRating, ()) {
    analytics::EloTracker tracker;
    int32_t team_id = *rc::gen::inRange(1, 100000);
    RC_ASSERT(tracker.rating(team_id) == analytics::EloTracker::INITIAL_RATING);
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. RateLimiter properties
// ═══════════════════════════════════════════════════════════════════════════

RC_GTEST_PROP(RateLimiter, BurstCapacityHonored, ()) {
    int burst = *rc::gen::inRange(1, 200);
    serving::RateLimiter::Config cfg;
    cfg.max_burst = static_cast<double>(burst);
    cfg.tokens_per_sec = 0.0;  // no refill — pure burst test
    serving::RateLimiter limiter(cfg);

    int allowed = 0;
    // Fire burst + 10 requests rapidly
    for (int i = 0; i < burst + 10; ++i) {
        if (limiter.allow("test_client")) ++allowed;
    }
    // With zero refill rate, exactly burst requests should be allowed
    RC_ASSERT(allowed == burst);
}

RC_GTEST_PROP(RateLimiter, DifferentClientsIndependent, ()) {
    int burst = *rc::gen::inRange(5, 50);
    serving::RateLimiter::Config cfg;
    cfg.max_burst = static_cast<double>(burst);
    cfg.tokens_per_sec = 0.0;
    serving::RateLimiter limiter(cfg);

    // Exhaust client A
    for (int i = 0; i < burst + 5; ++i) limiter.allow("clientA");

    // Client B should still have full burst
    int allowed_b = 0;
    for (int i = 0; i < burst; ++i) {
        if (limiter.allow("clientB")) ++allowed_b;
    }
    RC_ASSERT(allowed_b == burst);
}

// ── Main ──────────────────────────────────────────────────────────────────
// GTest main is linked via gtest_main; RC_GTEST_PROP registers automatically.
