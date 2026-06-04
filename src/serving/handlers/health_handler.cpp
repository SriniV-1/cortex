#include "serving/handlers/health_handler.hpp"
#include "analytics/GameStateIndex.hpp"
#include "analytics/EloTracker.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

using json = nlohmann::json;

namespace cortex::serving::handlers {

void handle_health(Request& /*req*/, Response& res, ServerContext& ctx) {
    bool all_healthy = true;

    json checks = json::object();

    // Database check
    bool db_up = ctx.db && ctx.db->is_open();
    checks["database"] = {{"status", db_up ? "up" : "down"}};
    if (!db_up) all_healthy = false;

    // Redis cache check
    bool redis_up = ctx.cache && ctx.cache->connected();
    checks["redis"] = {{"status", redis_up ? "up" : "down"}};
    if (!redis_up) all_healthy = false;

    // Similarity index check
    if (ctx.game_state_index) {
        bool idx_loaded = ctx.game_state_index->loaded();
        json idx_status = {{"status", idx_loaded ? "loaded" : "building"}};
        if (idx_loaded) {
            idx_status["vectors"]  = ctx.game_state_index->size();
            idx_status["build_ms"] = ctx.game_state_index->build_ms();
        }
        checks["similarity_index"] = idx_status;
        if (!idx_loaded) all_healthy = false;
    } else {
        checks["similarity_index"] = {{"status", "unavailable"}};
        all_healthy = false;
    }

    // Elo tracker check
    if (ctx.elo_tracker) {
        bool elo_built = ctx.elo_tracker->built();
        json elo_status = {{"status", elo_built ? "loaded" : "building"}};
        if (elo_built) {
            elo_status["games_processed"] = ctx.elo_tracker->games_processed();
            elo_status["build_ms"]        = ctx.elo_tracker->build_ms();
        }
        checks["elo_tracker"] = elo_status;
        if (!elo_built) all_healthy = false;
    } else {
        checks["elo_tracker"] = {{"status", "unavailable"}};
        all_healthy = false;
    }

    // Circuit breaker states
    json cbs = json::object();
    if (ctx.redis_circuit_breaker) {
        cbs["redis"] = ctx.redis_circuit_breaker->state_string();
    } else {
        cbs["redis"] = "unknown";
    }

    json response = {
        {"status", all_healthy ? "healthy" : "degraded"},
        {"checks", checks},
        {"circuit_breakers", cbs}
    };

    int code = all_healthy ? 200 : 503;
    res.json(response.dump(), code);
}

void handle_ready(Request& /*req*/, Response& res, ServerContext& ctx) {
    bool db_up      = ctx.db && ctx.db->is_open();
    bool idx_loaded = ctx.game_state_index && ctx.game_state_index->loaded();

    bool ready = db_up && idx_loaded;

    json response = {
        {"ready", ready},
        {"database", db_up ? "connected" : "disconnected"},
        {"similarity_index", idx_loaded ? "loaded" : "not_loaded"}
    };

    int code = ready ? 200 : 503;
    res.json(response.dump(), code);
}

} // namespace cortex::serving::handlers
