// Inference Traffic Fabric - capability labelling vocabulary.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_CAPABILITY_HPP
#define ITF_CAPABILITY_HPP

#include <cstdint>
#include <string_view>

namespace itf {

/// Renders the fabric's capability labels. REAL means the capability was
/// exercised on this host; SYNTHETIC means it is a declared simulator model;
/// UNSUPPORTED means it is not available here at all.
enum class CapabilityLabel : std::uint16_t {
  Unknown = 0,
  Real = 1,
  Synthetic = 2,
  Unsupported = 3,
  Count = 4,
};

[[nodiscard]] std::string_view to_string(CapabilityLabel value) noexcept;

}  // namespace itf

#endif  // ITF_CAPABILITY_HPP
