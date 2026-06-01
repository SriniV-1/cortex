#pragma once
#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"

namespace cortex::serving::handlers {
void handle_game_stats(Request& req, Response& res, ServerContext& ctx);
void handle_player_season(Request& req, Response& res, ServerContext& ctx);
void handle_api_stats(Request& req, Response& res, ServerContext& ctx);
void handle_leaderboard(Request& req, Response& res, ServerContext& ctx);
} // namespace cortex::serving::handlers
