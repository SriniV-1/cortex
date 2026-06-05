#include "serving/handlers/auth_handler.hpp"
#include "serving/Auth.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace cortex::serving::handlers {

void handle_auth_token(Request& req, Response& res, ServerContext& ctx) {
    // Parse request body
    json body;
    try {
        body = json::parse(req.body);
    } catch (...) {
        res.error("invalid JSON body", 400);
        return;
    }

    if (!body.contains("api_key") || !body["api_key"].is_string()) {
        res.error("missing or invalid api_key field", 400);
        return;
    }

    std::string provided_key = body["api_key"].get<std::string>();

    if (ctx.api_key.empty() || provided_key != ctx.api_key) {
        res.error("invalid api_key", 401);
        return;
    }

    // Determine role — default to admin when authenticating via api_key.
    // Callers can request "viewer" explicitly.
    std::string role = "admin";
    if (body.contains("role") && body["role"].is_string()) {
        std::string requested = body["role"].get<std::string>();
        if (requested == "viewer" || requested == "admin") {
            role = requested;
        }
    }

    int expiry_sec = 3600;
    std::string token = cortex::serving::create_token("api", role, ctx.jwt_secret, expiry_sec);

    json response = {
        {"token", token},
        {"expires_in", expiry_sec}
    };
    res.json(response.dump());
}

} // namespace cortex::serving::handlers
