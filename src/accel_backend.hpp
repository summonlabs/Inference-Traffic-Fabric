// Inference Traffic Fabric - accelerator backend boundary.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This header is deliberately C-compatible: it is included both by the CUDA
// translation unit (compiled by nvcc) and by the C++20 host translation unit.

#ifndef ITF_ACCEL_BACKEND_HPP
#define ITF_ACCEL_BACKEND_HPP

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ItfAccelProbeRaw {
  /* 0 when the probe executed on a physical device, non-zero otherwise. */
  int status;
  int compute_major;
  int compute_minor;
  int ran_kernel;
  int ran_memory_copy;
  char device_name[256];
  char driver_version[64];
  char device_uuid[64];
  char error[256];
  unsigned long long device_memory_bytes;
  unsigned long long generation;
  unsigned long long bytes_verified;
  unsigned long long copy_nanos;
  unsigned long long bytes_per_second;
  unsigned int kernel_launches;
  unsigned int mismatches;
} ItfAccelProbeRaw;

/* Runs a bounded device query, one real kernel launch and one real
   device-to-host copy with verification. Returns 0 on success. */
int itf_accel_probe_raw(int run_kernel, ItfAccelProbeRaw* out);

#ifdef __cplusplus
}
#endif

#endif  // ITF_ACCEL_BACKEND_HPP
