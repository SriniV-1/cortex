// Unit tests for Phase 2 stream processing components.
// Tests: RingBuffer, StatAccumulator, StreamProcessor, WinProbModel.

#include "stream/RingBuffer.hpp"
#include "stream/StreamEvent.hpp"
#include "stream/StatAccumulator.hpp"
#include "stream/StreamProcessor.hpp"
#include "analytics/WinProbModel.hpp"

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <numeric>

using namespace cortex::stream;
using namespace cortex::analytics;

// ── Helper: build a minimal StreamEvent ───────────────────────────────────
static StreamEvent make_event(int32_t player_id, ActionType type,
                               bool shot_made = false,
                               int16_t score_home = 0, int16_t score_away = 0) {
    StreamEvent ev{};
    ev.player_id   = player_id;
    ev.team_id     = 1610612738;  // arbitrary team
    ev.action_type = type;
    ev.shot_made   = shot_made;
    ev.score_home  = score_home;
    ev.score_away  = score_away;
    ev.period      = 1;
    // Set game_id to a fixed value
    const char* gid = "0022300001";
    std::copy(gid, gid + 11, ev.game_id.begin());
    return ev;
}

// ── RingBuffer ─────────────────────────────────────────────────────────────

TEST(RingBuffer, RejectNonPowerOfTwo) {
    EXPECT_THROW(RingBuffer<int>(3), std::invalid_argument);
    EXPECT_THROW(RingBuffer<int>(0), std::invalid_argument);
    EXPECT_NO_THROW(RingBuffer<int>(4));
    EXPECT_NO_THROW(RingBuffer<int>(65536));
}

TEST(RingBuffer, PushPop) {
    RingBuffer<int> buf(4);
    EXPECT_TRUE(buf.empty());

    EXPECT_TRUE(buf.try_push(10));
    EXPECT_TRUE(buf.try_push(20));
    EXPECT_EQ(buf.size(), 2u);

    auto v1 = buf.try_pop();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 10);

    auto v2 = buf.try_pop();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 20);

    EXPECT_FALSE(buf.try_pop().has_value());
}

TEST(RingBuffer, FullRejection) {
    RingBuffer<int> buf(4);
    EXPECT_TRUE(buf.try_push(1));
    EXPECT_TRUE(buf.try_push(2));
    EXPECT_TRUE(buf.try_push(3));
    EXPECT_TRUE(buf.try_push(4));
    EXPECT_FALSE(buf.try_push(5));  // full
    EXPECT_TRUE(buf.full());
}

TEST(RingBuffer, SPSC_Concurrent) {
    RingBuffer<int64_t> buf(1024);
    constexpr int N = 100'000;
    std::atomic<int64_t> sum_produced{0}, sum_consumed{0};

    std::jthread producer([&] {
        for (int i = 0; i < N; ++i) {
            buf.push(i);
            sum_produced += i;
        }
    });

    std::jthread consumer([&] {
        int count = 0;
        while (count < N) {
            if (auto v = buf.try_pop()) {
                sum_consumed += *v;
                ++count;
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(sum_produced.load(), sum_consumed.load());
}

TEST(RingBuffer, WrapAround) {
    // Verify that index wrapping works correctly
    RingBuffer<int> buf(4);
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 4; ++i) EXPECT_TRUE(buf.try_push(i));
        for (int i = 0; i < 4; ++i) {
            auto v = buf.try_pop();
            ASSERT_TRUE(v.has_value());
            EXPECT_EQ(*v, i);
        }
    }
}

// ── StatAccumulator ────────────────────────────────────────────────────────

TEST(StatAccumulator, ShotCounting) {
    StatAccumulator acc;

    acc.process(make_event(23, ActionType::Shot2pt, true,  2, 0));   // made 2pt
    acc.process(make_event(23, ActionType::Shot2pt, false, 2, 0));   // missed 2pt
    acc.process(make_event(23, ActionType::Shot3pt, true,  5, 0));   // made 3pt
    acc.process(make_event(23, ActionType::FreeThrow, true, 6, 0));  // made FT

    auto s = acc.player_stats(23, "0022300001");
    EXPECT_EQ(s.points,   6);   // 2 + 3 + 1
    EXPECT_EQ(s.fga,      3);   // 3 field goal attempts (FT not counted)
    EXPECT_EQ(s.fgm,      2);   // 2 makes
    EXPECT_EQ(s.fta,      1);
    EXPECT_EQ(s.ftm,      1);
    EXPECT_FLOAT_EQ(s.fg_pct, 2.0f / 3.0f);
}

TEST(StatAccumulator, ReboundsAssistsTurnovers) {
    StatAccumulator acc;
    acc.process(make_event(11, ActionType::Rebound));
    acc.process(make_event(11, ActionType::Rebound));
    acc.process(make_event(11, ActionType::Assist));
    acc.process(make_event(11, ActionType::Turnover));
    acc.process(make_event(11, ActionType::Foul));

    auto s = acc.player_stats(11, "0022300001");
    EXPECT_EQ(s.rebounds,  2);
    EXPECT_EQ(s.assists,   1);
    EXPECT_EQ(s.turnovers, 1);
    EXPECT_EQ(s.fouls,     1);
}

TEST(StatAccumulator, ScoreTracking) {
    StatAccumulator acc;
    acc.process(make_event(0, ActionType::Shot2pt, true, 50, 47));
    acc.process(make_event(0, ActionType::Shot3pt, true, 50, 50));

    auto [home, away] = acc.score("0022300001");
    EXPECT_EQ(home, 50);
    EXPECT_EQ(away, 50);
}

TEST(StatAccumulator, RollingWindow) {
    StatAccumulator acc;
    // Feed 30 events — 20 made 2pt + 10 misses
    for (int i = 0; i < 20; ++i)
        acc.process(make_event(7, ActionType::Shot2pt, true));
    for (int i = 0; i < 10; ++i)
        acc.process(make_event(7, ActionType::Shot2pt, false));

    // Last 20 events: 10 makes, 10 misses
    auto r = acc.rolling_stats(7, "0022300001", 20);
    EXPECT_EQ(r.window_size, 20);
    EXPECT_EQ(r.fgm, 10);
    EXPECT_EQ(r.fga, 20);
    EXPECT_EQ(r.points, 20);  // 10 × 2pts
    EXPECT_FLOAT_EQ(r.fg_pct, 0.5f);

    // Smaller window: last 5 events (all misses)
    auto r5 = acc.rolling_stats(7, "0022300001", 5);
    EXPECT_EQ(r5.fgm, 0);
    EXPECT_EQ(r5.fga, 5);
}

TEST(StatAccumulator, MultiplePlayerIsolation) {
    StatAccumulator acc;
    acc.process(make_event(1, ActionType::Shot3pt, true));
    acc.process(make_event(2, ActionType::Shot2pt, true));
    acc.process(make_event(3, ActionType::Rebound));

    EXPECT_EQ(acc.player_stats(1, "0022300001").points, 3);
    EXPECT_EQ(acc.player_stats(2, "0022300001").points, 2);
    EXPECT_EQ(acc.player_stats(3, "0022300001").rebounds, 1);
    EXPECT_EQ(acc.player_stats(3, "0022300001").points, 0);
}

TEST(StatAccumulator, EventCount) {
    StatAccumulator acc;
    for (int i = 0; i < 100; ++i)
        acc.process(make_event(i % 10, ActionType::Shot2pt, i % 2 == 0));
    EXPECT_EQ(acc.event_count(), 100);
}

// ── StreamProcessor ────────────────────────────────────────────────────────

TEST(StreamProcessor, DrainAndStop) {
    RingBuffer<StreamEvent> buf(1024);
    StatAccumulator acc;
    StreamProcessor proc(buf, acc);

    std::atomic<int> cb_count{0};
    proc.start([&](const StreamEvent&) { ++cb_count; });

    // Push 500 events from this (producer) thread
    constexpr int N = 500;
    for (int i = 0; i < N; ++i)
        buf.push(make_event(i % 5, ActionType::Shot2pt, i % 2 == 0));

    // Wait until all events are drained
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (acc.event_count() < N &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    proc.stop();

    EXPECT_EQ(acc.event_count(), N);
    EXPECT_EQ(cb_count.load(), N);
    EXPECT_FALSE(proc.running());
}

TEST(StreamProcessor, MetricsReported) {
    RingBuffer<StreamEvent> buf(256);
    StatAccumulator acc;
    StreamProcessor proc(buf, acc);
    proc.start();

    for (int i = 0; i < 100; ++i)
        buf.push(make_event(1, ActionType::Rebound));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (acc.event_count() < 100 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    proc.stop();
    auto m = proc.metrics();
    EXPECT_EQ(m.events_processed, 100);
    EXPECT_GT(m.events_per_sec, 0.0);
}

// ── WinProbModel ───────────────────────────────────────────────────────────

TEST(WinProbModel, LoadAndPredict) {
    // Model path relative to build directory — adjust if running from elsewhere
    const std::string path = CMAKE_SOURCE_DIR "/data/models/win_prob.onnx";
    WinProbModel model(path);
    ASSERT_TRUE(model.loaded());

    // Tied game, Q2: should be close to 0.5
    float p_tied = model.predict({0.0f, 2.0f, 600.0f, 1.0f, 0.0f});
    EXPECT_GT(p_tied, 0.3f);
    EXPECT_LT(p_tied, 0.7f);

    // Home team up by 20 late in Q4: should be high
    float p_winning = model.predict({20.0f, 4.0f, 60.0f, 1.0f, 1.0f});
    EXPECT_GT(p_winning, p_tied);

    // Home team down by 20 late: should be low
    float p_losing = model.predict({-20.0f, 4.0f, 60.0f, 1.0f, -1.0f});
    EXPECT_LT(p_losing, p_tied);

    // All probabilities in [0, 1]
    EXPECT_GE(p_tied,    0.0f);  EXPECT_LE(p_tied,    1.0f);
    EXPECT_GE(p_winning, 0.0f);  EXPECT_LE(p_winning, 1.0f);
    EXPECT_GE(p_losing,  0.0f);  EXPECT_LE(p_losing,  1.0f);
}

TEST(WinProbModel, BadPathThrows) {
    EXPECT_THROW(WinProbModel("/nonexistent/path/model.onnx"), std::runtime_error);
}
