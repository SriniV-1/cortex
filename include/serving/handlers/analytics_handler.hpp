#pragma once
#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"

namespace cortex::serving::handlers {
void handle_elo_rankings(Request& req, Response& res, ServerContext& ctx);
void handle_elo_history(Request& req, Response& res, ServerContext& ctx);
void handle_similarity(Request& req, Response& res, ServerContext& ctx);
void handle_index_status(Request& req, Response& res, ServerContext& ctx);
} // namespace cortex::serving::handlers
