// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "accel_backend.hpp"
#include "itf/accel.hpp"

namespace itf::accel {

bool supported_at_build() noexcept { return true; }

Result<AcceleratorProbe> probe(bool run_kernel) noexcept {
  AcceleratorProbe result;
  result.supported_at_build = true;
  result.toolchain = "cuda";
  ItfAccelProbeRaw raw{};
  const int status = itf_accel_probe_raw(run_kernel ? 1 : 0, &raw);
  result.device_name = raw.device_name;
  result.driver_version = raw.driver_version;
  result.device_uuid = raw.device_uuid;
  result.compute_capability_major = raw.compute_major;
  result.compute_capability_minor = raw.compute_minor;
  result.device_memory_bytes = raw.device_memory_bytes;
  result.device_generation = raw.generation;
  result.bytes_verified = raw.bytes_verified;
  result.copy_nanos = raw.copy_nanos;
  result.bytes_per_second = raw.bytes_per_second;
  result.kernel_launches = raw.kernel_launches;
  result.mismatches = raw.mismatches;
  result.ran_kernel = raw.ran_kernel != 0;
  result.ran_memory_copy = raw.ran_memory_copy != 0;
  if (status != 0) {
    result.label = CapabilityLabel::Unsupported;
    result.unavailable_reason =
        raw.error[0] != '\0' ? std::string(raw.error) : std::string("accelerator probe failed");
    return Result<AcceleratorProbe>::failure(StatusCode::Unsupported, result.unavailable_reason);
  }
  result.label = CapabilityLabel::Real;
  return Result<AcceleratorProbe>::success(std::move(result));
}

}  // namespace itf::accel
