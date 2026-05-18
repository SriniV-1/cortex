#pragma once
// StreamEvent — the unit of work that flows through the ring buffer.
// Kept small (fits in ~128 bytes) to minimize cache pressure.

#include <cstdint>
#include <string_view>
#include <array>

namespace cortex::stream {

// Action type codes — avoids repeated string comparisons in the hot path.
enum class ActionType : uint8_t {
    Unknown       = 0,
    Shot2pt       = 1,
    Shot3pt       = 2,
    FreeThrow     = 3,
    Rebound       = 4,
    Assist        = 5,
    Turnover      = 6,
    Foul          = 7,
    Substitution  = 8,
    Timeout       = 9,
    JumpBall      = 10,
    Period        = 11,
    Other         = 127,
};

// Decode NBA action_type string to enum (used during ingestion).
inline ActionType parse_action_type(std::string_view s) noexcept {
    if (s == "2pt")         return ActionType::Shot2pt;
    if (s == "3pt")         return ActionType::Shot3pt;
    if (s == "freethrow")   return ActionType::FreeThrow;
    if (s == "rebound")     return ActionType::Rebound;
    if (s == "assist")      return ActionType::Assist;
    if (s == "turnover")    return ActionType::Turnover;
    if (s == "foul")        return ActionType::Foul;
    if (s == "substitution")return ActionType::Substitution;
    if (s == "timeout")     return ActionType::Timeout;
    if (s == "jumpball")    return ActionType::JumpBall;
    if (s == "period")      return ActionType::Period;
    return ActionType::Other;
}

// Inline string storage — avoids heap allocation per event.
// 31 chars + null terminator fits most action descriptions and game IDs.
template<size_t N>
using InlineStr = std::array<char, N>;

struct alignas(64) StreamEvent {
    // ── Identity ─────────────────────────────────────────────────────────
    int64_t     event_id       = 0;
    int64_t     order_number   = 0;
    int64_t     occurred_ms    = 0;    // Unix epoch milliseconds

    // ── Game context ─────────────────────────────────────────────────────
    InlineStr<12> game_id      = {};   // "0022300001\0"
    int32_t     home_team_id   = 0;
    int32_t     away_team_id   = 0;

    // ── Play data ─────────────────────────────────────────────────────────
    int32_t     player_id      = 0;    // 0 = team event
    int32_t     team_id        = 0;
    ActionType  action_type    = ActionType::Unknown;
    uint8_t     period         = 0;
    int16_t     score_home     = 0;
    int16_t     score_away     = 0;
    float       x              = 0.0f;
    float       y              = 0.0f;

    // ── Shot result (only valid for Shot* types) ──────────────────────────
    bool        shot_made      = false;
    bool        is_fast_break  = false;

    // Padding to reach 128 bytes (keeps pairs of events in one cache line).
    uint8_t _pad[2]            = {};
};

static_assert(sizeof(StreamEvent) <= 128, "StreamEvent too large for cache efficiency");

} // namespace cortex::stream
