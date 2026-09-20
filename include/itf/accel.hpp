// Inference Traffic Fabric - accelerator capability probe.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_ACCEL_HPP
#define ITF_ACCEL_HPP

#include <cstdint>
#include <string>

#include "itf/capability.hpp"
#include "itf/error.hpp"

namespace itf::accel {

struct AcceleratorProbe {
  /// REAL when the probe executed on a physical device on this host,
  /// UNSUPPORTED when no device or no toolkit was available. SYNTHETIC is
  /// never reported by this probe: it describes declared simulator topologies,
  /// not accelerator execution.
  CapabilityLabel label = CapabilityLabel::Unsupported;
  bool supported_at_build = false;
  bool ran_kernel = false;
  bool ran_memory_copy = false;
  std::string device_name;
  std::string driver_version;
  std::string toolchain;
  std::string unavailable_reason;
  int compute_capability_major = 0;
  int compute_capability_minor = 0;
  std::uint64_t device_memory_bytes = 0;
  std::uint64_t device_generation = 0;
  std::uint64_t bytes_verified = 0;
  std::uint64_t copy_nanos = 0;
  std::uint64_t bytes_per_second = 0;
  std::uint32_t kernel_launches = 0;
  std::uint32_t mismatches = 0;
  std::string device_uuid;

  [[nodiscard]] std::string to_string() const;
};

/// True when this build contains a compiled accelerator backend.
[[nodiscard]] bool supported_at_build() noexcept;

/// Attempts a bounded device query, a real kernel launch and a real
/// device-to-host copy. When run_kernel is false only the query and copy run.
/// A device generation bound here is REAL evidence: it is derived from the
/// physical device identity, not from configuration.
[[nodiscard]] Result<AcceleratorProbe> probe(bool run_kernel) noexcept;

}  // namespace itf::accel

#endif  // ITF_ACCEL_HPP
