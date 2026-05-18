#pragma once
// Thin wrapper around spdlog — one include to get a named logger.
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>
#include <mutex>
#include <string>

namespace cortex {

inline std::shared_ptr<spdlog::logger> get_logger(const std::string& name) {
    // Mutex guards the get→create sequence against concurrent thread registration.
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    auto existing = spdlog::get(name);
    if (existing) return existing;
    auto logger = spdlog::stdout_color_mt(name);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    return logger;
}

} // namespace cortex
