#include "stream/StatAccumulator.hpp"
#include "common/Logger.hpp"
#include <cstring>
#include <functional>

namespace cortex::stream {

// ── Key helpers ────────────────────────────────────────────────────────────
// Compound key: player_id in upper 32 bits, game counter in lower 32 bits.
// game_id is 10 digits; we use the last 7 digits as the game counter.
int64_t StatAccumulator::player_key(int32_t player_id,
                                    std::string_view game_id) const noexcept {
    int32_t game_suffix = 0;
    if (game_id.size() >= 7) {
        // Last 7 chars of "0022300001" → "2300001" → 2300001
        for (size_t i = game_id.size() - 7; i < game_id.size(); ++i) {
            game_suffix = game_suffix * 10 + (game_id[i] - '0');
        }
    }
    return (static_cast<int64_t>(player_id) << 32)
         | static_cast<int64_t>(static_cast<uint32_t>(game_suffix));
}

int64_t StatAccumulator::team_key(int32_t team_id,
                                  std::string_view game_id) const noexcept {
    return player_key(team_id, game_id);  // same encoding
}

// ── Event processing (hot path — called from consumer thread) ─────────────
void StatAccumulator::process(const StreamEvent& ev) noexcept {
    event_count_.fetch_add(1, std::memory_order_relaxed);

    // Update score — atomic snapshot for any reader
    {
        std::string gid(ev.game_id.data());
        std::unique_lock lock(score_mu_);
        scores_[gid] = {ev.score_home, ev.score_away};
        game_last_seen_[gid] = Clock::now();
    }

    // Skip non-scoring/non-stat events
    if (ev.action_type == ActionType::Period   ||
        ev.action_type == ActionType::Timeout  ||
        ev.action_type == ActionType::Unknown  ||
        ev.action_type == ActionType::Other) {
        return;
    }

    std::string_view game_id(ev.game_id.data());

    // ── Player stats ────────────────────────────────────────────────────
    if (ev.player_id > 0) {
        int64_t pk = player_key(ev.player_id, game_id);

        // Optimistic: try shared lock first for atomic stat updates
        // (most entries already exist and atomics are safe under shared lock)
        {
            std::shared_lock lock(player_mu_);
            auto it = player_stats_.find(pk);
            if (it != player_stats_.end()) {
                auto& s = it->second;
                switch (ev.action_type) {
                case ActionType::Shot2pt:
                    s.fga.fetch_add(1, std::memory_order_relaxed);
                    if (ev.shot_made) {
                        s.fgm.fetch_add(1, std::memory_order_relaxed);
                        s.points.fetch_add(2, std::memory_order_relaxed);
                    }
                    break;
                case ActionType::Shot3pt:
                    s.fga.fetch_add(1, std::memory_order_relaxed);
                    if (ev.shot_made) {
                        s.fgm.fetch_add(1, std::memory_order_relaxed);
                        s.points.fetch_add(3, std::memory_order_relaxed);
                    }
                    break;
                case ActionType::FreeThrow:
                    s.fta.fetch_add(1, std::memory_order_relaxed);
                    if (ev.shot_made) {
                        s.ftm.fetch_add(1, std::memory_order_relaxed);
                        s.points.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                case ActionType::Rebound:
                    s.rebounds.fetch_add(1, std::memory_order_relaxed);
                    break;
                case ActionType::Assist:
                    s.assists.fetch_add(1, std::memory_order_relaxed);
                    break;
                case ActionType::Turnover:
                    s.turnovers.fetch_add(1, std::memory_order_relaxed);
                    break;
                case ActionType::Foul:
                    s.fouls.fetch_add(1, std::memory_order_relaxed);
                    break;
                default:
                    break;
                }
                // shared lock released here; upgrade to exclusive for rolling log
            }
        }

        // Exclusive lock: update rolling log (deque not safe under shared lock)
        // Also handles new player×game insertion.
        {
            std::unique_lock lock(player_mu_);
            // Insert stats entry if this is the first event for this player×game
            auto [it, inserted] = player_stats_.emplace(std::piecewise_construct,
                                                        std::forward_as_tuple(pk),
                                                        std::forward_as_tuple());
            if (inserted) {
                // First-ever event: apply stats that weren't applied in shared path
                auto& s = it->second;
                if (ev.action_type == ActionType::Shot2pt) {
                    s.fga++;
                    if (ev.shot_made) { s.fgm++; s.points += 2; }
                } else if (ev.action_type == ActionType::Shot3pt) {
                    s.fga++;
                    if (ev.shot_made) { s.fgm++; s.points += 3; }
                } else if (ev.action_type == ActionType::FreeThrow) {
                    s.fta++;
                    if (ev.shot_made) { s.ftm++; s.points++; }
                } else if (ev.action_type == ActionType::Rebound)  { s.rebounds++;  }
                else if (ev.action_type == ActionType::Assist)     { s.assists++;   }
                else if (ev.action_type == ActionType::Turnover)   { s.turnovers++; }
                else if (ev.action_type == ActionType::Foul)       { s.fouls++;     }
            }
            // Append to rolling log
            auto& log = rolling_log_[pk];
            log.push_back({ev.action_type, ev.shot_made});
            if (static_cast<int>(log.size()) > kMaxRolling) log.pop_front();
        }
    }
}

// ── Rolling window query ───────────────────────────────────────────────────
StatAccumulator::RollingSnapshot StatAccumulator::rolling_stats(
        int32_t player_id, std::string_view game_id, int window) const {
    int64_t pk = player_key(player_id, game_id);
    std::shared_lock lock(player_mu_);

    auto it = rolling_log_.find(pk);
    if (it == rolling_log_.end()) return {};

    const auto& log = it->second;
    // Look at the last `window` entries (or fewer if not enough events yet)
    int start = std::max(0, static_cast<int>(log.size()) - window);

    RollingSnapshot snap{};
    snap.window_size = static_cast<int>(log.size()) - start;

    for (int i = start; i < static_cast<int>(log.size()); ++i) {
        const auto& e = log[i];
        switch (e.action) {
        case ActionType::Shot2pt:
            snap.fga++;
            if (e.shot_made) { snap.fgm++; snap.points += 2; }
            break;
        case ActionType::Shot3pt:
            snap.fga++;
            if (e.shot_made) { snap.fgm++; snap.points += 3; }
            break;
        case ActionType::FreeThrow:
            if (e.shot_made) snap.points++;
            break;
        case ActionType::Rebound:   snap.rebounds++;  break;
        case ActionType::Assist:    snap.assists++;   break;
        case ActionType::Turnover:  snap.turnovers++; break;
        default: break;
        }
    }

    snap.fg_pct   = snap.fga > 0 ? static_cast<float>(snap.fgm) / snap.fga : 0.0f;
    snap.momentum = snap.window_size > 0
                  ? static_cast<float>(snap.points) / snap.window_size * 10.0f
                  : 0.0f;
    return snap;
}

// ── Reader API ─────────────────────────────────────────────────────────────
PlayerGameStats::Snapshot StatAccumulator::player_stats(
        int32_t player_id, std::string_view game_id) const {
    int64_t pk = player_key(player_id, game_id);
    std::shared_lock lock(player_mu_);
    auto it = player_stats_.find(pk);
    if (it == player_stats_.end()) return {};
    return it->second.snapshot();
}

std::pair<int16_t, int16_t> StatAccumulator::score(
        std::string_view game_id) const noexcept {
    std::string gid(game_id);
    std::shared_lock lock(score_mu_);
    auto it = scores_.find(gid);
    return it != scores_.end() ? it->second : std::make_pair<int16_t, int16_t>(0, 0);
}

void StatAccumulator::reset() noexcept {
    {
        std::unique_lock l1(player_mu_);
        player_stats_.clear();
        rolling_log_.clear();
    }
    {
        std::unique_lock l2(team_mu_);
        team_stats_.clear();
    }
    {
        std::unique_lock l3(score_mu_);
        scores_.clear();
        game_last_seen_.clear();
    }
    event_count_.store(0, std::memory_order_relaxed);
}

// ── Eviction ──────────────────────────────────────────────────────────────

void StatAccumulator::evict_game(std::string_view game_id) {
    // Collect all compound keys that belong to this game.
    // The lower 32 bits of the compound key encode the game suffix.
    int32_t game_suffix = 0;
    if (game_id.size() >= 7) {
        for (size_t i = game_id.size() - 7; i < game_id.size(); ++i)
            game_suffix = game_suffix * 10 + (game_id[i] - '0');
    }
    const uint32_t suffix = static_cast<uint32_t>(game_suffix);

    auto matches_game = [suffix](int64_t key) {
        return static_cast<uint32_t>(key) == suffix;
    };

    {
        std::unique_lock lock(player_mu_);
        std::erase_if(player_stats_, [&](const auto& kv) { return matches_game(kv.first); });
        std::erase_if(rolling_log_,  [&](const auto& kv) { return matches_game(kv.first); });
    }
    {
        std::unique_lock lock(team_mu_);
        std::erase_if(team_stats_, [&](const auto& kv) { return matches_game(kv.first); });
    }
    {
        std::string gid(game_id);
        std::unique_lock lock(score_mu_);
        scores_.erase(gid);
        game_last_seen_.erase(gid);
    }
}

size_t StatAccumulator::evict_stale(std::chrono::seconds max_age) {
    auto cutoff = Clock::now() - max_age;
    std::vector<std::string> stale_games;

    {
        std::shared_lock lock(score_mu_);
        for (const auto& [gid, ts] : game_last_seen_) {
            if (ts < cutoff) stale_games.push_back(gid);
        }
    }

    for (const auto& gid : stale_games)
        evict_game(gid);

    if (!stale_games.empty()) {
        auto log = cortex::get_logger("accumulator");
        log->info("Evicted {} stale games (>{} sec idle)", stale_games.size(), max_age.count());
    }

    return stale_games.size();
}

} // namespace cortex::stream
