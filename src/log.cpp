// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/log.hpp"

#include <cstdio>
#include <string>

namespace itf {
namespace {

std::string_view level_label(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kError: return "error";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kInfo: return "info";
    case LogLevel::kDebug: return "debug";
  }
  return "unknown";
}

char to_lower(char value) noexcept {
  return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

}  // namespace

Logger::Logger() = default;

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::set_level(LogLevel level) noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  level_ = level;
}

LogLevel Logger::level() const noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  return level_;
}

void Logger::set_sink(Sink sink) {
  std::lock_guard<std::mutex> guard(mutex_);
  sink_ = std::move(sink);
}

bool Logger::parse_level(std::string_view text, LogLevel& out) noexcept {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char value : text) lowered.push_back(to_lower(value));
  if (lowered == "error") {
    out = LogLevel::kError;
    return true;
  }
  if (lowered == "warn" || lowered == "warning") {
    out = LogLevel::kWarn;
    return true;
  }
  if (lowered == "info") {
    out = LogLevel::kInfo;
    return true;
  }
  if (lowered == "debug") {
    out = LogLevel::kDebug;
    return true;
  }
  return false;
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message) {
  Sink sink;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (static_cast<int>(level) > static_cast<int>(level_)) return;
    sink = sink_;
  }
  if (sink) {
    sink(level, component, message);
    return;
  }
  std::string line;
  line.reserve(component.size() + message.size() + 16);
  line.append("[");
  line.append(level_label(level));
  line.append("] ");
  if (!component.empty()) {
    line.append(component);
    line.append(": ");
  }
  line.append(message);
  line.push_back('\n');
  (void)std::fwrite(line.data(), 1, line.size(), stderr);
  (void)std::fflush(stderr);
}

void log_error(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::kError, component, message);
}
void log_warn(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::kWarn, component, message);
}
void log_info(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::kInfo, component, message);
}
void log_debug(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::kDebug, component, message);
}

}  // namespace itf
