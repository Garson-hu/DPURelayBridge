#pragma once

// Define ACTIVE_LEVEL before include spdlog, otherwise SPDLOG_INFO macro will not work
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

inline void init_logger() {
    // Set the log format similar to simple_RDMA with color
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
    spdlog::set_level(spdlog::level::debug);
}