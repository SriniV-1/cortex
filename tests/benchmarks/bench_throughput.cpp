// Throughput benchmark for the SPSC stream processing pipeline.
// Target: ≥ 1M events/sec sustained on Apple M-series hardware.
//
// Measures end-to-end throughput: producer pushes StreamEvents into the
// RingBuffer; StreamProcessor drains and accumulates stats.

#include "stream/RingBuffer.hpp"
#include "stream/StreamEvent.hpp"
#include "stream/StreamProcessor.hpp"
#include "stream/StatAccumulator.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cstring>

using namespace cortex::stream;
using Clock = std::chrono::steady_clock;

// ── Event factory ─────────────────────────────────────────────────────────────
static StreamEvent make_event(int idx) noexcept {
    StreamEvent ev{};
    ev.player_id   = static_cast<int32_t>((idx % 10) + 1);
    ev.team_id     = 1610612738;
    ev.action_type = static_cast<ActionType>((idx % 4) + 1);  // Shot2pt..Assist
    ev.shot_made   = (idx % 3 == 0);
    ev.score_home  = static_cast<int16_t>(idx % 120);
    ev.score_away  = static_cast<int16_t>(idx % 115);
    ev.period      = static_cast<int8_t>((idx / 100'000) % 4 + 1);
    const char* gid = "0022300001";
    std::copy(gid, gid + 11, ev.game_id.begin());
    return ev;
}

// ── Single run: returns events/sec ───────────────────────────────────────────
static double run_once(int64_t n_events, int buf_size = 65536) {
    RingBuffer<StreamEvent> buf(buf_size);
    StatAccumulator acc;
    StreamProcessor proc(buf, acc);
    proc.start();

    auto t0 = Clock::now();

    // Producer: tight push loop with spin-wait on full buffer
    for (int64_t i = 0; i < n_events; ++i) {
        while (!buf.try_push(make_event(static_cast<int>(i)))) {
            // spin — buffer momentarily full
#if defined(__aarch64__)
            __asm__ volatile("yield" ::: "memory");
#else
            __asm__ volatile("pause" ::: "memory");
#endif
        }
    }

    // Wait for consumer to drain
    auto deadline = Clock::now() + std::chrono::seconds(30);
    while (acc.event_count() < n_events &&
           Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    auto t1 = Clock::now();
    proc.stop();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    return static_cast<double>(acc.event_count()) / secs;
}

int main() {
    std::printf("═══════════════════════════════════════════════════════\n");
    std::printf("  Cortex Stream Throughput Benchmark\n");
    std::printf("  Target: ≥ 1,000,000 events/sec\n");
    std::printf("═══════════════════════════════════════════════════════\n\n");

    // ── Warm-up ──────────────────────────────────────────────────────────────
    std::printf("  Warming up...\n");
    run_once(200'000);

    // ── Measurement: 5 × 2M events ───────────────────────────────────────────
    constexpr int    kRuns    = 5;
    constexpr int64_t kEvents = 2'000'000;

    std::vector<double> results;
    results.reserve(kRuns);

    for (int r = 0; r < kRuns; ++r) {
        double eps = run_once(kEvents);
        results.push_back(eps);
        std::printf("  run %d/%d  →  %'.0f ev/s\n", r + 1, kRuns, eps);
    }

    std::sort(results.begin(), results.end());
    double mean  = std::accumulate(results.begin(), results.end(), 0.0) / kRuns;
    double p50   = results[kRuns / 2];
    double p05   = results[0];           // worst run
    double best  = results[kRuns - 1];

    std::printf("\n───────────────────────────────────────────────────────\n");
    std::printf("  mean   : %'.0f ev/s\n", mean);
    std::printf("  median : %'.0f ev/s\n", p50);
    std::printf("  best   : %'.0f ev/s\n", best);
    std::printf("  worst  : %'.0f ev/s\n", p05);
    std::printf("───────────────────────────────────────────────────────\n");

    constexpr double kTarget = 1'000'000.0;
    bool pass = (p05 >= kTarget);   // even worst run must meet target

    std::printf("  Target (1M ev/s worst-run): %s\n",
                pass ? "PASS ✓" : "FAIL ✗");
    std::printf("═══════════════════════════════════════════════════════\n\n");

    return pass ? 0 : 1;
}
