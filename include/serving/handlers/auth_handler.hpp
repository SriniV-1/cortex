#pragma once
#include "serving/Request.hpp"
#include "serving/Response.hpp"
#include "serving/ServerContext.hpp"

namespace cortex::serving::handlers {
// POST /api/auth/token — exchange an API key for a JWT.
void handle_auth_token(Request& req, Response& res, ServerContext& ctx);
} // namespace cortex::serving::handlers
