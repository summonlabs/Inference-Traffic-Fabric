// Inference Traffic Fabric - CRC-32C (Castagnoli) integrity checking.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_CRC32C_HPP
#define ITF_CRC32C_HPP

#include <cstdint>

#include "itf/bytes.hpp"

namespace itf {

/// CRC-32C with the reflected polynomial 0x1EDC6F41 (Castagnoli), initial value
/// 0xFFFFFFFF, final xor 0xFFFFFFFF. Used for frame and snapshot integrity.
class Crc32c {
 public:
  Crc32c() noexcept = default;

  void update(ByteSpan data) noexcept;
  void update(std::uint8_t byte) noexcept;

  [[nodiscard]] std::uint32_t value() const noexcept { return state_ ^ 0xFFFFFFFFU; }

  [[nodiscard]] static std::uint32_t compute(ByteSpan data) noexcept {
    Crc32c crc;
    crc.update(data);
    return crc.value();
  }

 private:
  std::uint32_t state_ = 0xFFFFFFFFU;
};

}  // namespace itf

#endif  // ITF_CRC32C_HPP
