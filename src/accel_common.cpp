// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "itf/accel.hpp"

namespace itf {

std::string_view to_string(CapabilityLabel value) noexcept {
  switch (value) {
    case CapabilityLabel::Unknown: return "UNKNOWN";
    case CapabilityLabel::Real: return "REAL";
    case CapabilityLabel::Synthetic: return "SYNTHETIC";
    case CapabilityLabel::Unsupported: return "UNSUPPORTED";
    case CapabilityLabel::Count: return "COUNT";
  }
  return "UNKNOWN";
}

}  // namespace itf

namespace itf::accel {

std::string AcceleratorProbe::to_string() const {
  std::string out;
  out.reserve(384);
  out.append("label=");
  out.append(itf::to_string(label));
  out.append(" supported_at_build=");
  out.append(supported_at_build ? "true" : "false");
  out.append(" ran_kernel=");
  out.append(ran_kernel ? "true" : "false");
  out.append(" ran_memory_copy=");
  out.append(ran_memory_copy ? "true" : "false");
  if (!device_name.empty()) {
    out.append(" device=\"");
    out.append(device_name);
    out.push_back('"');
  }
  if (compute_capability_major != 0 || compute_capability_minor != 0) {
    out.append(" compute_capability=");
    out.append(std::to_string(compute_capability_major));
    out.push_back('.');
    out.append(std::to_string(compute_capability_minor));
  }
  if (device_memory_bytes != 0) {
    out.append(" device_memory_bytes=");
    out.append(std::to_string(device_memory_bytes));
  }
  if (device_generation != 0) {
    out.append(" device_generation=");
    out.append(std::to_string(device_generation));
  }
  if (bytes_verified != 0) {
    out.append(" bytes_verified=");
    out.append(std::to_string(bytes_verified));
    out.append(" copy_nanos=");
    out.append(std::to_string(copy_nanos));
    out.append(" bytes_per_second=");
    out.append(std::to_string(bytes_per_second));
  }
  if (kernel_launches != 0) {
    out.append(" kernel_launches=");
    out.append(std::to_string(kernel_launches));
    out.append(" mismatches=");
    out.append(std::to_string(mismatches));
  }
  if (!driver_version.empty()) {
    out.append(" driver=\"");
    out.append(driver_version);
    out.push_back('"');
  }
  if (!toolchain.empty()) {
    out.append(" toolchain=\"");
    out.append(toolchain);
    out.push_back('"');
  }
  if (!device_uuid.empty()) {
    out.append(" device_uuid=");
    out.append(device_uuid);
  }
  if (!unavailable_reason.empty()) {
    out.append(" unavailable_reason=\"");
    out.append(unavailable_reason);
    out.push_back('"');
  }
  return out;
}

}  // namespace itf::accel
