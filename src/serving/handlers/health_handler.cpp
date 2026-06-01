#include "serving/handlers/health_handler.hpp"

namespace cortex::serving::handlers {

void handle_health(Request& /*req*/, Response& res, ServerContext& /*ctx*/) {
    res.json(R"({"status":"ok","service":"cortex"})");
}

} // namespace cortex::serving::handlers
