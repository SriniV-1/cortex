#include "serving/handlers/ws_handler.hpp"
#include "serving/HttpServer.hpp"
#include "serving/HttpUtils.hpp"

#include <algorithm>

namespace cortex::serving::handlers {

void handle_ws_upgrade(Request& req, Response& res, ServerContext& ctx) {
    // WebSocket upgrade is special — it needs direct Connection access.
    if (!ctx.connection) {
        res.json(R"({"error":"internal error"})", 500);
        return;
    }

    std::string game_id = req.param("gameId");

    std::string upgrade = get_header(req.headers_raw, "Upgrade");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
    if (upgrade != "websocket") {
        res.json(R"({"error":"WebSocket upgrade required"})", 400);
        return;
    }
    std::string ws_key = get_header(req.headers_raw, "Sec-WebSocket-Key");
    if (ws_key.empty()) {
        res.json(R"({"error":"missing Sec-WebSocket-Key"})", 400);
        return;
    }

    // Delegate to Connection's upgrade method directly.
    // Mark response as handled so dispatch code doesn't send a normal response.
    ctx.connection->upgrade_to_websocket(ws_key, game_id);
    res.handled = true;
}

} // namespace cortex::serving::handlers
