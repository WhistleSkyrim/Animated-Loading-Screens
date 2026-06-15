#include "Logging/Log.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <mutex>

#if ALS_HAS_SPDLOG
#    include <spdlog/sinks/basic_file_sink.h>
#    include <spdlog/sinks/null_sink.h>
#    include <spdlog/sinks/stdout_color_sinks.h>
#endif

namespace
{
    std::mutex g_loggerMutex;

#if ALS_HAS_SPDLOG
    std::shared_ptr<spdlog::logger> g_logger;
    std::shared_ptr<spdlog::logger> g_diagnosticLogger;

    [[nodiscard]] spdlog::level::level_enum ParseLevel(std::string_view level)
    {
        std::string value(level);
        std::ranges::transform(value, value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (value == "trace") {
            return spdlog::level::trace;
        }
        if (value == "debug") {
            return spdlog::level::debug;
        }
        if (value == "warn" || value == "warning") {
            return spdlog::level::warn;
        }
        if (value == "error") {
            return spdlog::level::err;
        }
        if (value == "critical") {
            return spdlog::level::critical;
        }
        if (value == "off") {
            return spdlog::level::off;
        }
        return spdlog::level::info;
    }
#else
    ALS::Log::Level g_level = ALS::Log::Level::Info;
    std::ofstream g_file;
    std::ofstream g_diagnosticFile;
    bool g_console = false;

    [[nodiscard]] ALS::Log::Level ParseLevel(std::string_view level)
    {
        std::string value(level);
        std::ranges::transform(value, value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (value == "trace") {
            return ALS::Log::Level::Trace;
        }
        if (value == "debug") {
            return ALS::Log::Level::Debug;
        }
        if (value == "warn" || value == "warning") {
            return ALS::Log::Level::Warn;
        }
        if (value == "error") {
            return ALS::Log::Level::Error;
        }
        return ALS::Log::Level::Info;
    }

    [[nodiscard]] int Rank(ALS::Log::Level level)
    {
        return static_cast<int>(level);
    }

    [[nodiscard]] const char* LevelName(ALS::Log::Level level)
    {
        switch (level) {
        case ALS::Log::Level::Trace:
            return "trace";
        case ALS::Log::Level::Debug:
            return "debug";
        case ALS::Log::Level::Info:
            return "info";
        case ALS::Log::Level::Warn:
            return "warn";
        case ALS::Log::Level::Error:
            return "error";
        }
        return "info";
    }
#endif
}

namespace ALS::Log
{
    void InitializeFileLogger(const std::filesystem::path& logPath, std::string_view level)
    {
        std::lock_guard lock(g_loggerMutex);

        if (logPath.has_parent_path()) {
            std::filesystem::create_directories(logPath.parent_path());
        }

#if ALS_HAS_SPDLOG
        g_logger = spdlog::basic_logger_mt("AnimatedLoadingScreens", logPath.string(), true);
        g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        g_logger->set_level(ParseLevel(level));
        g_logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(g_logger);
#else
        g_file.open(logPath, std::ios::trunc);
        g_console = false;
        g_level = ParseLevel(level);
#endif
    }

    void InitializeDiagnosticLogger(const std::filesystem::path& logPath)
    {
        std::lock_guard lock(g_loggerMutex);

        if (logPath.has_parent_path()) {
            std::filesystem::create_directories(logPath.parent_path());
        }

#if ALS_HAS_SPDLOG
        g_diagnosticLogger = spdlog::basic_logger_mt("AnimatedLoadingScreens.Diagnostics", logPath.string(), true);
        g_diagnosticLogger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
        g_diagnosticLogger->set_level(spdlog::level::trace);
        g_diagnosticLogger->flush_on(spdlog::level::info);
#else
        g_diagnosticFile.open(logPath, std::ios::trunc);
#endif
    }

    void InitializeConsoleLoggerForTests(std::string_view level)
    {
        std::lock_guard lock(g_loggerMutex);
#if ALS_HAS_SPDLOG
        g_logger = spdlog::stdout_color_mt("AnimatedLoadingScreens.Tests");
        g_logger->set_level(ParseLevel(level));
        spdlog::set_default_logger(g_logger);
#else
        g_file.close();
        g_diagnosticFile.close();
        g_console = true;
        g_level = ParseLevel(level);
#endif
    }

    void SetLevel(std::string_view level)
    {
#if ALS_HAS_SPDLOG
        Get()->set_level(ParseLevel(level));
#else
        std::lock_guard lock(g_loggerMutex);
        g_level = ParseLevel(level);
#endif
    }

#if ALS_HAS_SPDLOG
    std::shared_ptr<spdlog::logger> Get()
    {
        std::lock_guard lock(g_loggerMutex);
        if (!g_logger) {
            g_logger = std::make_shared<spdlog::logger>(
                "AnimatedLoadingScreens.Null",
                std::make_shared<spdlog::sinks::null_sink_mt>());
            g_logger->set_level(spdlog::level::off);
        }
        return g_logger;
    }

    std::shared_ptr<spdlog::logger> GetDiagnostic()
    {
        std::lock_guard lock(g_loggerMutex);
        if (!g_diagnosticLogger) {
            g_diagnosticLogger = std::make_shared<spdlog::logger>(
                "AnimatedLoadingScreens.Diagnostics.Null",
                std::make_shared<spdlog::sinks::null_sink_mt>());
            g_diagnosticLogger->set_level(spdlog::level::off);
        }
        return g_diagnosticLogger;
    }
#else
    void Write(Level level, std::string_view message)
    {
        std::lock_guard lock(g_loggerMutex);
        if (Rank(level) < Rank(g_level)) {
            return;
        }

        if (g_console) {
            std::cerr << '[' << LevelName(level) << "] " << message << '\n';
            return;
        }
        if (g_file) {
            g_file << '[' << LevelName(level) << "] " << message << '\n';
            if (Rank(level) >= Rank(Level::Warn)) {
                g_file.flush();
            }
        }
    }

    void WriteDiagnostic(std::string_view message)
    {
        std::lock_guard lock(g_loggerMutex);
        if (g_console) {
            std::cerr << "[diag] " << message << '\n';
            return;
        }
        if (g_diagnosticFile) {
            g_diagnosticFile << message << '\n';
            g_diagnosticFile.flush();
        }
    }
#endif
}
