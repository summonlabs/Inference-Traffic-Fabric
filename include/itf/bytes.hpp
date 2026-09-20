// Inference Traffic Fabric - byte spans and checked arithmetic.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_BYTES_HPP
#define ITF_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace itf {

using ByteSpan = std::span<const std::uint8_t>;
using MutableByteSpan = std::span<std::uint8_t>;

/// Overflow-checked addition. Returns false and leaves out untouched on wrap.
[[nodiscard]] inline bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
  out = a + b;
  return true;
}

/// Overflow-checked multiplication.
[[nodiscard]] inline bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) return false;
  out = a * b;
  return true;
}

/// Narrowing conversion that refuses to truncate.
[[nodiscard]] inline bool narrow_to_size(std::uint64_t value, std::size_t& out) noexcept {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return false;
  out = static_cast<std::size_t>(value);
  return true;
}

[[nodiscard]] inline std::uint64_t bytes_of(ByteSpan span) noexcept {
  return static_cast<std::uint64_t>(span.size());
}

[[nodiscard]] inline ByteSpan as_bytes(std::string_view text) noexcept {
  return ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

/// FNV-1a 64-bit digest used for opaque, non-reversible metadata labels such as
/// tenant and model tags. This is a labelling function, not cryptography, and
/// is documented as such at every use site.
[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view text) noexcept {
  std::uint64_t hash = 0xCBF29CE484222325ULL;
  for (const char raw : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
    hash *= 0x100000001B3ULL;
  }
  return hash;
}

}  // namespace itf

#endif  // ITF_BYTES_HPP
