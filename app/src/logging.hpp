#pragma once

#include <source_location>
#include <utility>
#include <chrono>

#include "error.hpp"

// Global console and file logging

namespace alfred::logging {
    inline constexpr const char* FILE = "alfred.log";

    enum class Severity {
        Debug,
        Information,
        Warning,
        Error,
        Critical
    };

    constexpr const char* to_string(Severity severity) {
        switch (severity) {
            case Severity::Debug:
                return "Debug";
            case Severity::Information:
                return "Information";
            case Severity::Warning:
                return "Warning";
            case Severity::Error:
                return "Error";
            case Severity::Critical:
                return "Critical";
        }

        std::unreachable();
    }

    void initialize();
    void uninitialize();

    namespace chrono = std::chrono;
    using TimeOfDay = chrono::hh_mm_ss<chrono::seconds>;

    void println_console(Severity severity, const std::source_location& location, TimeOfDay time_of_day, const std::string& message);
    void println_file(Severity severity, const std::source_location& location, TimeOfDay time_of_day, const std::string& message);

    template<Severity severity, typename... Args>
    void log(const std::source_location& location, std::format_string<Args...> fmt, Args&&... args) {
        const auto time = chrono::system_clock::now();
        const auto time_of_day = TimeOfDay(chrono::floor<chrono::seconds>(time - chrono::floor<chrono::days>(time)));
        const auto message = std::format(std::move(fmt), std::forward<Args>(args)...);

        println_console(severity, location, time_of_day, message);
        println_file(severity, location, time_of_day, message);
    }

    template<typename... Args>
    struct debug {
        explicit debug(std::format_string<Args...> fmt, Args&&... args, const std::source_location& location = std::source_location::current()) {
            log<Severity::Debug>(location, std::move(fmt), std::forward<Args>(args)...);
        }
    };

    template<typename... Args>
    debug(std::format_string<Args...> fmt, Args&&... args) -> debug<Args...>;

    template<typename... Args>
    struct information {
        explicit information(std::format_string<Args...> fmt, Args&&... args, const std::source_location& location = std::source_location::current()) {
            log<Severity::Information>(location, std::move(fmt), std::forward<Args>(args)...);
        }
    };

    template<typename... Args>
    information(std::format_string<Args...> fmt, Args&&... args) -> information<Args...>;

    template<typename... Args>
    struct warning {
        explicit warning(std::format_string<Args...> fmt, Args&&... args, const std::source_location& location = std::source_location::current()) {
            log<Severity::Warning>(location, std::move(fmt), std::forward<Args>(args)...);
        }
    };

    template<typename... Args>
    warning(std::format_string<Args...> fmt, Args&&... args) -> warning<Args...>;

    template<typename... Args>
    struct error {
        explicit error(std::format_string<Args...> fmt, Args&&... args, const std::source_location& location = std::source_location::current()) {
            log<Severity::Error>(location, std::move(fmt), std::forward<Args>(args)...);
        }
    };

    template<typename... Args>
    error(std::format_string<Args...> fmt, Args&&... args) -> error<Args...>;

    template<typename... Args>
    struct critical {
        explicit critical(std::format_string<Args...> fmt, Args&&... args, const std::source_location& location = std::source_location::current()) {
            log<Severity::Critical>(location, std::move(fmt), std::forward<Args>(args)...);
        }
    };

    template<typename... Args>
    critical(std::format_string<Args...> fmt, Args&&... args) -> critical<Args...>;

    struct LoggingError : alfred::error::Error {
        using Error::Error;

        ALFRED_ERROR_NAME(LoggingError)
    };
}

#ifdef ALFRED_DISTRIBUTION
    #define LOG_DEBUG(...) (void) 0
    #define LOG_INFORMATION(...) (void) 0
    #define LOG_WARNING(...) (void) 0
    #define LOG_ERROR(...) (void) 0
    #define LOG_CRITICAL(...) (void) 0
#else
    #define LOG_DEBUG(...) logging::debug(__VA_ARGS__)
    #define LOG_INFORMATION(...) logging::information(__VA_ARGS__)
    #define LOG_WARNING(...) logging::warning(__VA_ARGS__)
    #define LOG_ERROR(...) logging::error(__VA_ARGS__)
    #define LOG_CRITICAL(...) logging::critical(__VA_ARGS__)
#endif
