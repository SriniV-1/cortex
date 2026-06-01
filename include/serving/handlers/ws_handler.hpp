#pragma once
#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"

namespace cortex::serving::handlers {
void handle_ws_upgrade(Request& req, Response& res, ServerContext& ctx);
} // namespace cortex::serving::handlers
