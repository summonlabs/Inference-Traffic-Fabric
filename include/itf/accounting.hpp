// Inference Traffic Fabric - per-class and per-request network accounting.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_ACCOUNTING_HPP
#define ITF_ACCOUNTING_HPP

#include <array>
#include <cstdint>
#include <string>

#include "itf/ids.hpp"
#include "itf/traffic.hpp"

namespace itf {

struct ClassAccounting {
  std::uint64_t allowed = 0;
  std::uint64_t degraded = 0;
  std::uint64_t deferred = 0;
  std::uint64_t rejected = 0;
  std::uint64_t bytes_allowed = 0;
  std::uint64_t bytes_deferred = 0;
  std::uint64_t bytes_rejected = 0;
  std::uint32_t active_flows = 0;
  std::uint32_t peak_active_flows = 0;
  std::uint32_t starvation_admissions = 0;
};

struct RequestAccounting {
  RequestId request;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::uint32_t decisions = 0;
  std::uint32_t defers = 0;
  std::uint32_t rejections = 0;
  std::uint32_t active_flows = 0;
  bool closed = false;
};

struct AccountingSnapshot {
  std::array<ClassAccounting, kTrafficClassCount> per_class{};
  std::uint64_t requests_registered = 0;
  std::uint64_t requests_completed = 0;
  std::uint64_t requests_cancelled = 0;
  std::uint64_t requests_failed = 0;
  std::uint64_t requests_interrupted = 0;
  std::uint64_t requests_evicted = 0;
  std::uint64_t attempts_registered = 0;
  std::uint64_t attempts_completed = 0;
  std::uint64_t attempts_failed = 0;
  std::uint64_t attempts_superseded = 0;
  std::uint64_t completions_committed = 0;
  std::uint64_t completions_suppressed = 0;
  std::uint64_t completions_refused = 0;
  std::uint64_t transfers_started = 0;
  std::uint64_t transfers_completed = 0;
  std::uint64_t transfers_refused = 0;
  std::uint32_t active_flows_total = 0;
  std::uint32_t peak_active_flows_total = 0;
  std::uint32_t pending_deferrals = 0;
  std::size_t live_requests = 0;
  std::size_t retained_requests = 0;

  /// True when every flow opened against the fabric has been released. This is
  /// the accounting-closure predicate asserted by closure tests.
  [[nodiscard]] bool closed() const noexcept { return active_flows_total == 0; }

  [[nodiscard]] std::string to_string() const;
};

}  // namespace itf

#endif  // ITF_ACCOUNTING_HPP
