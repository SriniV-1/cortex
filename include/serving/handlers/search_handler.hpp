#pragma once
#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"

namespace cortex::serving::handlers {
void handle_search_players(Request& req, Response& res, ServerContext& ctx);
void handle_search_games(Request& req, Response& res, ServerContext& ctx);
void handle_search_events(Request& req, Response& res, ServerContext& ctx);
} // namespace cortex::serving::handlers
