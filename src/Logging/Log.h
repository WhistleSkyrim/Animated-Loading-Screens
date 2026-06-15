#pragma once

#include <filesystem>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>

#if ALS_HAS_SPDLOG
#    include <spdlog/spdlog.h>
#endif

namespace ALS::Log
{
    void InitializeFileLogger(const std::filesystem::path& logPath, std::string_view level);
    void InitializeDiagnosticLogger(const std::filesystem::path& logPath);
    void InitializeConsoleLoggerForTests(std::string_view level = "debug");
    void SetLevel(std::string_view level);

#if ALS_HAS_SPDLOG
    [[nodiscard]] std::shared_ptr<spdlog::logger> Get();
    [[nodiscard]] std::shared_ptr<spdlog::logger> GetDiagnostic();

    template <class... Args>
    void trace(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        Get()->trace(fmt, std::forward<Args>(args)...);
    }

    template <class... Args>
    void debug(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        Get()->debug(fmt, std::forward<Args>(args)...);
    }

    template <class... Args>
    void info(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        Get()->info(fmt, std::forward<Args>(args)...);
    }

    template <class... Args>
    void warn(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        Get()->warn(fmt, std::forward<Args>(args)...);
    }

    template <class... Args>
    void error(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        Get()->error(fmt, std::forward<Args>(args)...);
    }

    template <class... Args>
    void diagnostic(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        GetDiagnostic()->info(fmt, std::forward<Args>(args)...);
    }
#else
    enum class Level
    {
        Trace,
        Debug,
        Info,
        Warn,
        Error
    };

    void Write(Level level, std::string_view message);
    void WriteDiagnostic(std::string_view message);

    namespace Detail
    {
        template <class T>
        std::string ToString(T&& value)
        {
            std::ostringstream stream;
            stream << std::forward<T>(value);
            return stream.str();
        }

        inline void ReplaceFirst(std::string& text, const std::string& value)
        {
            const auto pos = text.find("{}");
            if (pos != std::string::npos) {
                text.replace(pos, 2, value);
                return;
            }

            const auto open = text.find('{');
            const auto close = open == std::string::npos ? std::string::npos : text.find('}', open + 1);
            if (open != std::string::npos && close != std::string::npos) {
                text.replace(open, close - open + 1, value);
                return;
            }

            if (pos == std::string::npos) {
                text += " ";
                text += value;
                return;
            }
        }

        inline std::string Format(std::string_view fmt)
        {
            return std::string(fmt);
        }

        template <class First, class... Rest>
        std::string Format(std::string_view fmt, First&& first, Rest&&... rest)
        {
            auto text = std::string(fmt);
            ReplaceFirst(text, ToString(std::forward<First>(first)));
            if constexpr (sizeof...(Rest) > 0) {
                return Format(text, std::forward<Rest>(rest)...);
            } else {
                return text;
            }
        }
    }

    template <class... Args>
    void trace(std::string_view fmt, Args&&... args)
    {
        Write(Level::Trace, Detail::Format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void debug(std::string_view fmt, Args&&... args)
    {
        Write(Level::Debug, Detail::Format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void info(std::string_view fmt, Args&&... args)
    {
        Write(Level::Info, Detail::Format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void warn(std::string_view fmt, Args&&... args)
    {
        Write(Level::Warn, Detail::Format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void error(std::string_view fmt, Args&&... args)
    {
        Write(Level::Error, Detail::Format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void diagnostic(std::string_view fmt, Args&&... args)
    {
        WriteDiagnostic(Detail::Format(fmt, std::forward<Args>(args)...));
    }
#endif
}
