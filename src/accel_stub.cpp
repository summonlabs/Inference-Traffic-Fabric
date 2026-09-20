// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This translation unit is used when the build has no accelerator backend.
// It reports UNSUPPORTED and never fabricates accelerator evidence.

#include <string>

#include "itf/accel.hpp"

namespace itf::accel {

bool supported_at_build() noexcept { return false; }

Result<AcceleratorProbe> probe(bool run_kernel) noexcept {
  (void)run_kernel;
  AcceleratorProbe result;
  result.label = CapabilityLabel::Unsupported;
  result.supported_at_build = false;
  result.unavailable_reason =
      "this build was compiled without an accelerator backend, so no accelerator evidence is "
      "available";
  return Result<AcceleratorProbe>::failure(StatusCode::Unsupported, result.unavailable_reason);
}

}  // namespace itf::accel
