// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/time.hpp"

#include <chrono>

namespace itf {

SteadyClock::SteadyClock() noexcept = default;

std::int64_t SteadyClock::now_nanos() const noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

Clock& steady_clock_singleton() noexcept {
  static SteadyClock clock;
  return clock;
}

}  // namespace itf
