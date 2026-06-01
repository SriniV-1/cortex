#pragma once
// ServerContext — shared dependencies injected into route handlers.

#include "serving/RateLimiter.hpp"
#include "serving/RedisCache.hpp"
#include "stream/StatAccumulator.hpp"

// Forward declarations to avoid pulling heavy headers.
namespace pqxx { class connection; }
namespace cortex::analytics { class GameStateIndex; class EloTracker; }
namespace cortex::etl { class LiveIngestor; }

namespace cortex::serving {

// Forward-declare Connection for WebSocket upgrade handler.
class Connection;

struct ServerContext {
    cortex::stream::StatAccumulator*         accumulator       = nullptr;
    pqxx::connection*                        db                = nullptr;
    RedisCache*                              cache             = nullptr;
    const cortex::analytics::GameStateIndex* game_state_index  = nullptr;
    const cortex::analytics::EloTracker*     elo_tracker       = nullptr;
    const cortex::etl::LiveIngestor*         live_ingestor     = nullptr;
    RateLimiter*                             rate_limiter      = nullptr;
    std::string                              www_root;

    // For WebSocket upgrade, the handler needs direct Connection access.
    Connection*                              connection        = nullptr;
};

} // namespace cortex::serving
