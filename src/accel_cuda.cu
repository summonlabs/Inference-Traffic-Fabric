// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// REAL accelerator binding: this translation unit queries a physical device,
// launches a real kernel and performs a real device-to-host copy whose result
// is verified on the host. Nothing here is simulated.

#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "accel_backend.hpp"

namespace {

constexpr unsigned int kElements = 1U << 20;  // 4 MiB of 32-bit words.
constexpr unsigned int kThreads = 256U;

__global__ void pattern_kernel(unsigned int* data, unsigned int length) {
  const unsigned int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < length) {
    data[index] = index * 2654435761U + 0x9E3779B9U;
  }
}

unsigned long long mix(unsigned long long value) {
  value ^= value >> 30U;
  value *= 0xBF58476D1CE4E5B9ULL;
  value ^= value >> 27U;
  value *= 0x94D049BB133111EBULL;
  value ^= value >> 31U;
  return value;
}

void copy_text(char* destination, std::size_t capacity, const char* source) {
  if (capacity == 0) return;
  std::size_t index = 0;
  for (; source[index] != '\0' && index + 1 < capacity; ++index) destination[index] = source[index];
  destination[index] = '\0';
}

void report_error(ItfAccelProbeRaw* out, const char* message, int status) {
  out->status = status;
  copy_text(out->error, sizeof(out->error), message);
}

}  // namespace

extern "C" int itf_accel_probe_raw(int run_kernel, ItfAccelProbeRaw* out) {
  if (out == nullptr) return 1;
  std::memset(out, 0, sizeof(*out));

  int device_count = 0;
  const cudaError_t counted = cudaGetDeviceCount(&device_count);
  if (counted != cudaSuccess) {
    report_error(out, cudaGetErrorString(counted), 1);
    return 1;
  }
  if (device_count <= 0) {
    report_error(out, "no CUDA device is present", 2);
    return 2;
  }
  const cudaError_t selected = cudaSetDevice(0);
  if (selected != cudaSuccess) {
    report_error(out, cudaGetErrorString(selected), 3);
    return 3;
  }
  cudaDeviceProp properties{};
  const cudaError_t described = cudaGetDeviceProperties(&properties, 0);
  if (described != cudaSuccess) {
    report_error(out, cudaGetErrorString(described), 4);
    return 4;
  }
  copy_text(out->device_name, sizeof(out->device_name), properties.name);
  out->compute_major = properties.major;
  out->compute_minor = properties.minor;
  out->device_memory_bytes = static_cast<unsigned long long>(properties.totalGlobalMem);

  int driver_version = 0;
  if (cudaDriverGetVersion(&driver_version) == cudaSuccess) {
    char buffer[64] = {0};
    std::snprintf(buffer, sizeof(buffer), "driver API %d.%d", driver_version / 1000,
                  (driver_version % 1000) / 10);
    copy_text(out->driver_version, sizeof(out->driver_version), buffer);
  } else {
    copy_text(out->driver_version, sizeof(out->driver_version), "unknown");
  }

  unsigned long long generation = 0xCBF29CE484222325ULL;
  for (int index = 0; index < 16; ++index) {
    generation ^= static_cast<unsigned char>(properties.uuid.bytes[index]);
    generation *= 0x100000001B3ULL;
  }
  for (const char* cursor = properties.name; *cursor != '\0'; ++cursor) {
    generation ^= static_cast<unsigned char>(*cursor);
    generation *= 0x100000001B3ULL;
  }
  generation = mix(generation ^ (static_cast<unsigned long long>(properties.major) << 8U) ^
                   static_cast<unsigned long long>(properties.minor));
  out->generation = generation;
  char uuid_text[64] = {0};
  std::snprintf(uuid_text, sizeof(uuid_text),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                static_cast<unsigned char>(properties.uuid.bytes[0]),
                static_cast<unsigned char>(properties.uuid.bytes[1]),
                static_cast<unsigned char>(properties.uuid.bytes[2]),
                static_cast<unsigned char>(properties.uuid.bytes[3]),
                static_cast<unsigned char>(properties.uuid.bytes[4]),
                static_cast<unsigned char>(properties.uuid.bytes[5]),
                static_cast<unsigned char>(properties.uuid.bytes[6]),
                static_cast<unsigned char>(properties.uuid.bytes[7]),
                static_cast<unsigned char>(properties.uuid.bytes[8]),
                static_cast<unsigned char>(properties.uuid.bytes[9]),
                static_cast<unsigned char>(properties.uuid.bytes[10]),
                static_cast<unsigned char>(properties.uuid.bytes[11]),
                static_cast<unsigned char>(properties.uuid.bytes[12]),
                static_cast<unsigned char>(properties.uuid.bytes[13]),
                static_cast<unsigned char>(properties.uuid.bytes[14]),
                static_cast<unsigned char>(properties.uuid.bytes[15]));
  copy_text(out->device_uuid, sizeof(out->device_uuid), uuid_text);

  const std::size_t bytes = static_cast<std::size_t>(kElements) * sizeof(unsigned int);
  unsigned int* device_buffer = nullptr;
  const cudaError_t allocated = cudaMalloc(reinterpret_cast<void**>(&device_buffer), bytes);
  if (allocated != cudaSuccess) {
    report_error(out, cudaGetErrorString(allocated), 5);
    return 5;
  }
  const cudaError_t zeroed = cudaMemset(device_buffer, 0, bytes);
  if (zeroed != cudaSuccess) {
    (void)cudaFree(device_buffer);
    report_error(out, cudaGetErrorString(zeroed), 6);
    return 6;
  }

  if (run_kernel != 0) {
    const unsigned int blocks = (kElements + kThreads - 1U) / kThreads;
    pattern_kernel<<<blocks, kThreads>>>(device_buffer, kElements);
    const cudaError_t launched = cudaGetLastError();
    if (launched != cudaSuccess) {
      (void)cudaFree(device_buffer);
      report_error(out, cudaGetErrorString(launched), 7);
      return 7;
    }
    const cudaError_t synchronised = cudaDeviceSynchronize();
    if (synchronised != cudaSuccess) {
      (void)cudaFree(device_buffer);
      report_error(out, cudaGetErrorString(synchronised), 8);
      return 8;
    }
    out->kernel_launches = 1;
    out->ran_kernel = 1;
  }

  unsigned int* host_buffer = new (std::nothrow) unsigned int[kElements];
  if (host_buffer == nullptr) {
    (void)cudaFree(device_buffer);
    report_error(out, "host staging buffer allocation failed", 9);
    return 9;
  }
  const auto started = std::chrono::steady_clock::now();
  const cudaError_t copied =
      cudaMemcpy(host_buffer, device_buffer, bytes, cudaMemcpyDeviceToHost);
  const auto finished = std::chrono::steady_clock::now();
  if (copied != cudaSuccess) {
    delete[] host_buffer;
    (void)cudaFree(device_buffer);
    report_error(out, cudaGetErrorString(copied), 10);
    return 10;
  }
  const unsigned long long elapsed =
      static_cast<unsigned long long>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  out->copy_nanos = elapsed;
  out->bytes_verified = static_cast<unsigned long long>(bytes);
  out->bytes_per_second =
      elapsed == 0 ? 0ULL : (static_cast<unsigned long long>(bytes) * 1000000000ULL) / elapsed;
  out->ran_memory_copy = 1;

  unsigned int mismatches = 0;
  if (run_kernel != 0) {
    for (unsigned int index = 0; index < kElements; ++index) {
      const unsigned int expected = index * 2654435761U + 0x9E3779B9U;
      if (host_buffer[index] != expected) ++mismatches;
    }
  }
  out->mismatches = mismatches;
  delete[] host_buffer;
  const cudaError_t freed = cudaFree(device_buffer);
  if (freed != cudaSuccess) {
    report_error(out, cudaGetErrorString(freed), 11);
    return 11;
  }
  if (mismatches != 0) {
    report_error(out, "device output did not match the expected pattern", 12);
    return 12;
  }
  out->status = 0;
  return 0;
}
