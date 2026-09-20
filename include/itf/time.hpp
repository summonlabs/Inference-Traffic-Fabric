// Inference Traffic Fabric - explicit time domain and clock injection.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_TIME_HPP
#define ITF_TIME_HPP

#include <chrono>
#include <cstdint>
#include <limits>

namespace itf {

/// Absolute instant in the coordinator's monotonic time domain, in
/// nanoseconds. Constructed unset by default; an unset instant means "no
/// deadline" and never silently compares as expired.
class Instant {
 public:
  constexpr Instant() noexcept = default;

  [[nodiscard]] static constexpr Instant at(std::int64_t nanos) noexcept {
    Instant value;
    value.nanos_ = nanos;
    return value;
  }

  [[nodiscard]] static constexpr Instant from_now(std::int64_t now_nanos,
                                                  std::int64_t delta_nanos) noexcept {
    if (delta_nanos <= 0) return Instant();
    if (now_nanos > std::numeric_limits<std::int64_t>::max() - delta_nanos) return Instant();
    return Instant::at(now_nanos + delta_nanos);
  }

  [[nodiscard]] constexpr bool is_set() const noexcept { return nanos_ != kUnset; }
  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }

  /// True only when a deadline is set and the clock has reached it.
  [[nodiscard]] constexpr bool expired_at(std::int64_t now_nanos) const noexcept {
    return is_set() && now_nanos >= nanos_;
  }

  /// Remaining nanoseconds; zero when unset or already expired.
  [[nodiscard]] constexpr std::int64_t remaining_at(std::int64_t now_nanos) const noexcept {
    if (!is_set() || now_nanos >= nanos_) return 0;
    return nanos_ - now_nanos;
  }

  friend constexpr bool operator==(const Instant& a, const Instant& b) noexcept {
    return a.nanos_ == b.nanos_;
  }
  friend constexpr bool operator!=(const Instant& a, const Instant& b) noexcept {
    return !(a == b);
  }
  friend constexpr bool operator<(const Instant& a, const Instant& b) noexcept {
    if (!a.is_set()) return false;
    if (!b.is_set()) return true;
    return a.nanos_ < b.nanos_;
  }

 private:
  static constexpr std::int64_t kUnset = std::numeric_limits<std::int64_t>::min();
  std::int64_t nanos_ = kUnset;
};

/// Injectable time source. Production code uses SteadyClock; tests and
/// deterministic property runs use VirtualClock so that deferral, starvation
/// and staleness decisions are exactly reproducible.
class Clock {
 public:
  Clock() = default;
  virtual ~Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  Clock(Clock&&) = delete;
  Clock& operator=(Clock&&) = delete;

  /// Monotonic nanoseconds. The value is meaningless outside this process,
  /// which is why deadlines are always carried as durations across the wire.
  [[nodiscard]] virtual std::int64_t now_nanos() const noexcept = 0;
};

class SteadyClock final : public Clock {
 public:
  SteadyClock() noexcept;
  [[nodiscard]] std::int64_t now_nanos() const noexcept override;
};

/// Deterministic clock. It never advances on its own; callers move it.
class VirtualClock final : public Clock {
 public:
  VirtualClock() noexcept = default;
  explicit VirtualClock(std::int64_t start_nanos) noexcept : now_(start_nanos) {}

  [[nodiscard]] std::int64_t now_nanos() const noexcept override { return now_; }

  void advance(std::int64_t delta_nanos) noexcept {
    if (delta_nanos > 0) now_ += delta_nanos;
  }
  void set(std::int64_t nanos) noexcept { now_ = nanos; }

 private:
  std::int64_t now_ = 0;
};

/// Process-wide default clock, used when a component is constructed without
/// an explicit time source. It is the steady monotonic clock.
[[nodiscard]] Clock& steady_clock_singleton() noexcept;

[[nodiscard]] inline std::int64_t to_nanos(std::chrono::nanoseconds value) noexcept {
  return value.count();
}

[[nodiscard]] inline std::int64_t millis_to_nanos(std::uint64_t millis) noexcept {
  constexpr std::uint64_t kMaxMillis = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000000);
  if (millis > kMaxMillis) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(millis) * 1000000;
}

}  // namespace itf

#endif  // ITF_TIME_HPP
