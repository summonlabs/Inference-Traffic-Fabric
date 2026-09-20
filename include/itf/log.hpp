// Inference Traffic Fabric - minimal levelled logging.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_LOG_HPP
#define ITF_LOG_HPP

#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace itf {

enum class LogLevel : int { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

/// Process-wide logger. The default level is kWarn so that library code never
/// writes to stderr unless something is actually wrong; tools raise the level
/// explicitly. No logging path is required for correctness.
class Logger {
 public:
  using Sink = std::function<void(LogLevel, std::string_view, std::string_view)>;

  [[nodiscard]] static Logger& instance();

  void set_level(LogLevel level) noexcept;
  [[nodiscard]] LogLevel level() const noexcept;
  void set_sink(Sink sink);

  void log(LogLevel level, std::string_view component, std::string_view message);

  /// Parses "error", "warn", "info", "debug" (case-insensitive). Returns false
  /// on anything else so callers can reject bad configuration.
  [[nodiscard]] static bool parse_level(std::string_view text, LogLevel& out) noexcept;

 private:
  Logger();

  mutable std::mutex mutex_;
  LogLevel level_ = LogLevel::kWarn;
  Sink sink_;
};

void log_error(std::string_view component, std::string_view message);
void log_warn(std::string_view component, std::string_view message);
void log_info(std::string_view component, std::string_view message);
void log_debug(std::string_view component, std::string_view message);

}  // namespace itf

#endif  // ITF_LOG_HPP
