#pragma once
#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"

namespace cortex::serving::handlers {
void handle_games_recent(Request& req, Response& res, ServerContext& ctx);
void handle_scoreboard(Request& req, Response& res, ServerContext& ctx);
} // namespace cortex::serving::handlers
