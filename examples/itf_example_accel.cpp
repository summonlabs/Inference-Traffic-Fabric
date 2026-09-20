// Inference Traffic Fabric - example: accelerator evidence labelling.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <string>

#include "itf/accel.hpp"

int main() {
  std::printf("accelerator backend compiled in: %s\n",
              itf::accel::supported_at_build() ? "yes" : "no");
  const auto probe = itf::accel::probe(true);
  if (!probe.ok()) {
    std::printf("accelerator label: %s\n",
                std::string(itf::to_string(probe.value().label)).c_str());
    std::printf("reason: %s\n", probe.status().message().c_str());
    std::printf(
        "Multi-node, RDMA, SmartNIC and NVLink topology dimensions are SYNTHETIC in this "
        "repository unless physical hardware is present.\n");
    return 0;
  }
  std::printf("%s\n", probe.value().to_string().c_str());
  std::printf("label: %s\n", std::string(itf::to_string(probe.value().label)).c_str());
  return 0;
}
