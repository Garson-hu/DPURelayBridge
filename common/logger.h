// #pragma once

// #include <spdlog/sinks/basic_file_sink.h>
// #include <spdlog/sinks/stdout_color_sinks.h>
// #include <spdlog/spdlog.h>

// #include <iostream>
// #include <string>

// /**
//  * Initialize the logging system
//  *
//  * @param console_level Minimum log level for console output
//  * @param file_level Minimum log level for file output
//  * @param log_filename Name of the log file (empty for no file logging)
//  */
// void init_logging(spdlog::level::level_enum console_level = spdlog::level::info,
//                   spdlog::level::level_enum file_level = spdlog::level::debug,
//                   const std::string& log_filename = "");

// /**
//  * Set the global log level
//  *
//  * @param level New log level
//  */
// inline void set_log_level(spdlog::level::level_enum level) {
//     spdlog::set_level(level);
// }

// /**
//  * Convert a string to log level
//  *
//  * @param level_str String representation of log level
//  * @return Corresponding log level enum value
//  */
// inline spdlog::level::level_enum str_to_log_level(
//     const std::string& level_str) {
//     if (level_str == "trace") return spdlog::level::trace;
//     if (level_str == "debug") return spdlog::level::debug;
//     if (level_str == "info") return spdlog::level::info;
//     if (level_str == "warn") return spdlog::level::warn;
//     if (level_str == "error") return spdlog::level::err;
//     if (level_str == "critical") return spdlog::level::critical;
//     if (level_str == "off") return spdlog::level::off;

//     return spdlog::level::info;  // Default
// }


// /**
//  * Initialize the logging system
//  *
//  * @param console_level Minimum log level for console output
//  * @param file_level Minimum log level for file output
//  * @param log_filename Name of the log file (empty for no file logging)
//  */
// void init_logging(spdlog::level::level_enum console_level,
//                   spdlog::level::level_enum file_level,
//                   const std::string& log_filename) {
//     try {
//         // Create console sink
//         auto console_sink =
//             std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
//         console_sink->set_level(console_level);
//         console_sink->set_pattern(
//             "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v [%s:%# %t]");

//         std::vector<spdlog::sink_ptr> sinks{console_sink};

//         // Create file sink if filename is provided
//         if (!log_filename.empty()) {
//             auto file_sink =
//                 std::make_shared<spdlog::sinks::basic_file_sink_mt>(
//                     log_filename, true);
//             file_sink->set_level(file_level);
//             file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v [%s:%# %t]");
//             sinks.push_back(file_sink);
//         }

//         // Create logger with both sinks
//         auto logger = std::make_shared<spdlog::logger>("rdma", sinks.begin(),
//                                                        sinks.end());
//         logger->set_level(spdlog::level::trace);  // Allow all messages to be
//                                                   // processed by the logger

//         // Set as default logger
//         spdlog::set_default_logger(logger);

//         // Flush automatically after each log message
//         spdlog::flush_on(spdlog::level::info);
//     } catch (const spdlog::spdlog_ex& ex) {
//         // We can't use SPDLOG macros here since the logger initialization
//         // failed Fall back to std::cerr, but use formatted output
//         std::cerr << "Log initialization failed: " << ex.what() << std::endl;
//     }
// }


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