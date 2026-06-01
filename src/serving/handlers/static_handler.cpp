#include "serving/handlers/static_handler.hpp"
#include "serving/HttpUtils.hpp"

namespace cortex::serving::handlers {

void handle_static(Request& req, Response& res, ServerContext& ctx) {
    std::string file_path;
    if (req.path == "/" || req.path == "/index.html") {
        file_path = ctx.www_root + "/index.html";
    } else if (req.path.rfind("/static/", 0) == 0) {
        file_path = ctx.www_root + req.path;
    }

    if (!file_path.empty()) {
        auto content = read_file(file_path);
        if (content) {
            res.send_file(*content, mime_type(file_path));
            return;
        }
    }

    // Not handled — leave res.handled = false so 404 is sent by dispatch.
}

} // namespace cortex::serving::handlers
