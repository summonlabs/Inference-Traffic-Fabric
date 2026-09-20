// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/crc32c.hpp"

#include <array>

namespace itf {
namespace {

constexpr std::uint32_t kReflectedPolynomial = 0x82F63B78U;

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256U; ++index) {
    std::uint32_t crc = index;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? ((crc >> 1U) ^ kReflectedPolynomial) : (crc >> 1U);
    }
    table[index] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();

}  // namespace

void Crc32c::update(std::uint8_t byte) noexcept {
  state_ = kTable[(state_ ^ byte) & 0xFFU] ^ (state_ >> 8U);
}

void Crc32c::update(ByteSpan data) noexcept {
  std::uint32_t state = state_;
  for (const std::uint8_t byte : data) {
    state = kTable[(state ^ byte) & 0xFFU] ^ (state >> 8U);
  }
  state_ = state;
}

}  // namespace itf
