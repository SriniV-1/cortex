#pragma once
// LiveIngestor — polls NBA S3 feed for live games and injects new play-by-play
// events into the StreamProcessor ring buffer.
//
// Behaviour:
//   - Every poll_interval_ms (default 30 000 ms) fetch the scoreboard.
//   - For each game with status == 2 (live), fetch its play-by-play.
//   - Track the highest action_number seen per game; push only new actions.
//   - Convert PlayAction → StreamEvent and try_push into the ring buffer.
//   - Stops cleanly when stop() is called.
//
// Thread model:
//   - Single jthread (the poll loop).
//   - Ring buffer is SPSC: LiveIngestor is the sole producer; StreamProcessor
//     is the sole consumer.

#include "etl/NBAClient.hpp"
#include "stream/RingBuffer.hpp"
#include "stream/StreamEvent.hpp"

#include <atomic>
#include <thread>
#include <unordered_map>
#include <cstdint>
#include <string>

namespace cortex::etl {

class LiveIngestor {
public:
    explicit LiveIngestor(
        cortex::stream::RingBuffer<cortex::stream::StreamEvent>& ring,
        int poll_interval_ms = 30'000,
        int request_delay_ms = 50);

    ~LiveIngestor();

    // Start the poll loop in a background jthread. Idempotent.
    void start();

    // Signal the loop to stop and join. Idempotent.
    void stop();

    bool running() const noexcept { return running_.load(); }

    // Total events injected since start.
    int64_t events_injected() const noexcept { return events_injected_.load(); }

private:
    void run();
    void poll_once();
    int  push_new_actions(const PlayByPlay& pbp);

    // Parse ISO 8601 clock string "PT11M45.00S" → seconds (720 max per period).
    static uint16_t parse_clock_secs(const std::string& clock) noexcept;

    // Convert a PlayAction + game context into a StreamEvent.
    static cortex::stream::StreamEvent make_event(
        const PlayAction&  action,
        const std::string& game_id,
        int32_t            home_team_id,
        int32_t            away_team_id) noexcept;

    cortex::stream::RingBuffer<cortex::stream::StreamEvent>& ring_;
    NBAClient                                                 client_;
    int                                                       poll_interval_ms_;

    std::atomic<bool>    stop_flag_{false};
    std::atomic<bool>    running_{false};
    std::atomic<int64_t> events_injected_{0};
    std::jthread         thread_;

    // Per-game watermark: highest order_number seen.
    std::unordered_map<std::string, int64_t>  watermarks_;
    // Per-game team IDs (populated from scoreboard).
    std::unordered_map<std::string, std::pair<int32_t,int32_t>> team_ids_;
};

} // namespace cortex::etl
