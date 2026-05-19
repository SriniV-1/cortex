#include "etl/LiveIngestor.hpp"
#include "common/Logger.hpp"
#include "stream/StreamEvent.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cortex::etl {

using cortex::stream::StreamEvent;
using cortex::stream::ActionType;
using cortex::stream::parse_action_type;

// ── Construction ───────────────────────────────────────────────────────────

LiveIngestor::LiveIngestor(
        cortex::stream::RingBuffer<StreamEvent>& ring,
        int poll_interval_ms,
        int request_delay_ms)
    : ring_(ring)
    , client_(request_delay_ms)
    , poll_interval_ms_(poll_interval_ms)
{}

LiveIngestor::~LiveIngestor() { stop(); }

// ── Lifecycle ──────────────────────────────────────────────────────────────

void LiveIngestor::start() {
    if (running_.load(std::memory_order_acquire)) return;
    stop_flag_.store(false, std::memory_order_release);
    thread_ = std::jthread([this] { run(); });
}

void LiveIngestor::stop() {
    stop_flag_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

// ── Poll loop ──────────────────────────────────────────────────────────────

void LiveIngestor::run() {
    auto log = cortex::get_logger("live_ingestor");
    running_.store(true, std::memory_order_release);
    log->info("LiveIngestor started (poll interval: {}ms)", poll_interval_ms_);

    while (!stop_flag_.load(std::memory_order_acquire)) {
        try {
            poll_once();
        } catch (const std::exception& e) {
            log->warn("poll_once error: {}", e.what());
        }

        // Sleep poll_interval_ms in 100ms increments so stop() is responsive.
        for (int elapsed = 0;
             elapsed < poll_interval_ms_
             && !stop_flag_.load(std::memory_order_acquire);
             elapsed += 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    running_.store(false, std::memory_order_release);
    log->info("LiveIngestor stopped. Total events injected: {}",
              events_injected_.load());
}

void LiveIngestor::poll_once() {
    auto log = cortex::get_logger("live_ingestor");

    auto games = client_.fetch_scoreboard();
    int live_count = 0;
    int new_events = 0;

    for (const auto& gs : games) {
        if (gs.status != 2) continue;  // 1=scheduled, 3=final
        ++live_count;

        // Cache team IDs from scoreboard.
        team_ids_[gs.game_id] = {gs.home_team_id, gs.away_team_id};

        auto pbp = client_.fetch_play_by_play(gs.game_id);
        if (!pbp) continue;

        new_events += push_new_actions(*pbp);
    }

    if (live_count > 0)
        log->info("poll: {} live games, {} new events pushed", live_count, new_events);
    else
        log->debug("poll: no live games");
}

int LiveIngestor::push_new_actions(const PlayByPlay& pbp) {
    auto log = cortex::get_logger("live_ingestor");

    int64_t  watermark = 0;
    auto wit = watermarks_.find(pbp.game_id);
    if (wit != watermarks_.end()) watermark = wit->second;

    int32_t home_id = 0, away_id = 0;
    if (auto tit = team_ids_.find(pbp.game_id); tit != team_ids_.end()) {
        home_id = tit->second.first;
        away_id = tit->second.second;
    }

    int pushed = 0;
    int64_t new_watermark = watermark;

    for (const auto& action : pbp.actions) {
        if (action.order_number <= watermark) continue;

        StreamEvent ev = make_event(action, pbp.game_id, home_id, away_id);

        // Spin-push with limited retries (ring should rarely be full
        // given 65536 capacity vs. ~1 action/sec live rate).
        int retries = 0;
        while (!ring_.try_push(ev) && retries < 1000) {
            std::this_thread::yield();
            ++retries;
        }
        if (retries == 1000) {
            log->warn("Ring buffer full — dropped event order={}", action.order_number);
            continue;
        }

        new_watermark = std::max(new_watermark, action.order_number);
        ++pushed;
        events_injected_.fetch_add(1, std::memory_order_relaxed);
    }

    if (pushed > 0) {
        watermarks_[pbp.game_id] = new_watermark;
        log->debug("game {}: pushed {} new actions (watermark: {} → {})",
                   pbp.game_id, pushed, watermark, new_watermark);
    }

    return pushed;
}

// ── Helpers ────────────────────────────────────────────────────────────────

// Parse "PT11M45.00S" → 705 (seconds). Returns 0 on parse error.
uint16_t LiveIngestor::parse_clock_secs(const std::string& clock) noexcept {
    if (clock.size() < 3 || clock[0] != 'P' || clock[1] != 'T') return 0;

    int minutes = 0, seconds = 0;
    // Try "PTmmMss.SSS"
    const char* p = clock.c_str() + 2;
    char* end = nullptr;

    long m = std::strtol(p, &end, 10);
    if (end && *end == 'M') {
        minutes = static_cast<int>(m);
        p = end + 1;
        long s = std::strtol(p, &end, 10);
        seconds = static_cast<int>(s);
    } else {
        // No minutes component: "PTss.SSS"
        long s = std::strtol(p, &end, 10);
        seconds = static_cast<int>(s);
    }

    uint32_t total = static_cast<uint32_t>(minutes * 60 + seconds);
    return static_cast<uint16_t>(std::min(total, static_cast<uint32_t>(65535)));
}

StreamEvent LiveIngestor::make_event(
        const PlayAction&  action,
        const std::string& game_id,
        int32_t            home_team_id,
        int32_t            away_team_id) noexcept {

    StreamEvent ev{};

    // Identity
    ev.event_id     = action.action_number;
    ev.order_number = action.order_number;

    // Game context
    const char* gid = game_id.c_str();
    std::size_t glen = std::min(game_id.size(), ev.game_id.size() - 1);
    std::copy(gid, gid + glen, ev.game_id.begin());
    ev.game_id[glen] = '\0';

    ev.home_team_id = home_team_id;
    ev.away_team_id = away_team_id;

    // Play data
    ev.player_id   = action.person_id;
    ev.team_id     = action.team_id;
    ev.action_type = parse_action_type(action.action_type);
    ev.period      = static_cast<uint8_t>(std::clamp(action.period, 0, 255));
    ev.score_home  = action.score_home;
    ev.score_away  = action.score_away;
    ev.x           = action.x;
    ev.y           = action.y;
    ev.clock_secs  = parse_clock_secs(action.clock);

    // Shot result: made if description exists and doesn't start with "MISS"
    if (ev.action_type == ActionType::Shot2pt
     || ev.action_type == ActionType::Shot3pt
     || ev.action_type == ActionType::FreeThrow) {
        ev.shot_made = !action.description.empty()
                    && action.description.compare(0, 4, "MISS") != 0;
    }

    return ev;
}

} // namespace cortex::etl
