// Inference Traffic Fabric - strongly typed identities and generations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_IDS_HPP
#define ITF_IDS_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace itf {

namespace detail {

[[nodiscard]] constexpr int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

[[nodiscard]] constexpr char hex_digit(unsigned value) noexcept {
  return static_cast<char>(value < 10U ? ('0' + static_cast<int>(value))
                                       : ('a' + static_cast<int>(value) - 10));
}

}  // namespace detail

/// 128-bit identity rendered as 32 lowercase hex characters. The all-zero
/// value is the nil identity and is never issued by the runtime.
template <class Tag>
class Id128 {
 public:
  constexpr Id128() noexcept = default;
  constexpr Id128(std::uint64_t high, std::uint64_t low) noexcept : high_(high), low_(low) {}

  [[nodiscard]] constexpr std::uint64_t high() const noexcept { return high_; }
  [[nodiscard]] constexpr std::uint64_t low() const noexcept { return low_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return high_ == 0 && low_ == 0; }

  [[nodiscard]] std::string to_string() const {
    std::string out;
    out.resize(32);
    for (int i = 0; i < 32; ++i) {
      const std::uint64_t source = (i < 16) ? high_ : low_;
      const unsigned shift = static_cast<unsigned>(15 - (i % 16)) * 4U;
      const unsigned nibble = static_cast<unsigned>((source >> shift) & 0xFU);
      out[static_cast<std::size_t>(i)] = detail::hex_digit(nibble);
    }
    return out;
  }

  /// Strict canonical parse: exactly 32 hex characters, no prefixes, no
  /// surrounding whitespace, no trailing garbage.
  [[nodiscard]] static std::optional<Id128> parse(std::string_view text) noexcept {
    if (text.size() != 32) return std::nullopt;
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    for (std::size_t i = 0; i < 32; ++i) {
      const int digit = detail::hex_value(text[i]);
      if (digit < 0) return std::nullopt;
      if (i < 16) {
        high = (high << 4U) | static_cast<std::uint64_t>(digit);
      } else {
        low = (low << 4U) | static_cast<std::uint64_t>(digit);
      }
    }
    return Id128(high, low);
  }

  friend constexpr bool operator==(const Id128& a, const Id128& b) noexcept {
    return a.high_ == b.high_ && a.low_ == b.low_;
  }
  friend constexpr bool operator!=(const Id128& a, const Id128& b) noexcept { return !(a == b); }
  friend constexpr bool operator<(const Id128& a, const Id128& b) noexcept {
    return a.high_ != b.high_ ? a.high_ < b.high_ : a.low_ < b.low_;
  }

 private:
  std::uint64_t high_ = 0;
  std::uint64_t low_ = 0;
};

/// Monotonic 64-bit generation counter. Value zero means "no generation has
/// been established yet" and is never a valid current generation.
template <class Tag>
class Generation {
 public:
  constexpr Generation() noexcept = default;
  explicit constexpr Generation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Generation from(std::uint64_t value) noexcept {
    return Generation(value);
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return value_ == 0; }

  /// Returns the next generation. Callers must not call this on the maximum
  /// value; generation space exhaustion is refused at the boundary instead.
  [[nodiscard]] constexpr Generation next() const noexcept { return Generation(value_ + 1); }

  friend constexpr bool operator==(const Generation& a, const Generation& b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(const Generation& a, const Generation& b) noexcept {
    return !(a == b);
  }
  friend constexpr bool operator<(const Generation& a, const Generation& b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator<=(const Generation& a, const Generation& b) noexcept {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>(const Generation& a, const Generation& b) noexcept {
    return a.value_ > b.value_;
  }
  friend constexpr bool operator>=(const Generation& a, const Generation& b) noexcept {
    return a.value_ >= b.value_;
  }

 private:
  std::uint64_t value_ = 0;
};

struct RequestIdTag;
struct AttemptIdTag;
struct StateTransferIdTag;
struct FlowIdTag;
struct SessionIdTag;
struct BootIdTag;
struct PeerIdTag;

using RequestId = Id128<RequestIdTag>;
using AttemptId = Id128<AttemptIdTag>;
using StateTransferId = Id128<StateTransferIdTag>;
using FlowId = Id128<FlowIdTag>;
using SessionId = Id128<SessionIdTag>;
using BootId = Id128<BootIdTag>;
using PeerId = Id128<PeerIdTag>;

struct RequestGenerationTag;
struct AttemptGenerationTag;
struct ModelGenerationTag;
struct StateGenerationTag;
struct RouteDecisionGenerationTag;
struct SloContractGenerationTag;
struct TopologyGenerationTag;
struct PolicyGenerationTag;
struct CoordinatorEpochTag;
struct AuthoritySequenceTag;

using RequestGeneration = Generation<RequestGenerationTag>;
using AttemptGeneration = Generation<AttemptGenerationTag>;
using ModelGeneration = Generation<ModelGenerationTag>;
using StateGeneration = Generation<StateGenerationTag>;
using RouteDecisionGeneration = Generation<RouteDecisionGenerationTag>;
using SloContractGeneration = Generation<SloContractGenerationTag>;
using TopologyGeneration = Generation<TopologyGenerationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;
using AuthoritySequence = Generation<AuthoritySequenceTag>;

}  // namespace itf

namespace std {

template <class Tag>
struct hash<itf::Id128<Tag>> {
  [[nodiscard]] std::size_t operator()(const itf::Id128<Tag>& id) const noexcept {
    std::uint64_t x =
        id.high() ^ (id.low() + 0x9E3779B97F4A7C15ULL + (id.high() << 6U) + (id.high() >> 2U));
    x ^= x >> 30U;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27U;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    return static_cast<std::size_t>(x);
  }
};

}  // namespace std

#endif  // ITF_IDS_HPP
