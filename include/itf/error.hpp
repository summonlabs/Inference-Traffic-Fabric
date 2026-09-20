// Inference Traffic Fabric - deterministic status codes and result plumbing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_ERROR_HPP
#define ITF_ERROR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace itf {

/// Deterministic, stable, machine-checkable outcome codes. No API in this
/// library reports failure through a bare boolean.
enum class StatusCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  OutOfRange = 2,
  NotFound = 3,
  AlreadyExists = 4,
  BoundsExceeded = 5,
  IntegrityMismatch = 6,
  MalformedInput = 7,
  TrailingGarbage = 8,
  UnsupportedVersion = 9,
  Unsupported = 10,
  StaleGeneration = 11,
  StaleEpoch = 12,
  StaleAuthority = 13,
  Cancelled = 14,
  Conflict = 15,
  NotAuthorized = 16,
  CapacityExhausted = 17,
  InvalidTransition = 18,
  PolicyInvalid = 19,
  EvidenceUnknown = 20,
  EvidenceStale = 21,
  RevalidationRequired = 22,
  ReplayRejected = 23,
  Closed = 24,
  ShuttingDown = 25,
  IoError = 26,
  Internal = 27,
  DuplicateIgnored = 28,
  Refused = 29,
};

[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

/// A status carries a deterministic code plus a human-readable detail string.
/// The code is the contract; the message is diagnostic only.
class Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  [[nodiscard]] static Status success() noexcept { return Status{}; }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Status& a, const Status& b) noexcept { return a.code_ == b.code_; }
  friend bool operator!=(const Status& a, const Status& b) noexcept { return !(a == b); }

 private:
  StatusCode code_ = StatusCode::Ok;
  std::string message_;
};

/// Result<T> is either a value or a failure status. It never holds both.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] static Result success(T value) { return Result(std::move(value)); }
  [[nodiscard]] static Result failure(Status status) { return Result(std::move(status)); }
  [[nodiscard]] static Result failure(StatusCode code, std::string message) {
    return Result(Status(code, std::move(message)));
  }

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] StatusCode code() const noexcept { return status_.code(); }

  [[nodiscard]] const T& value() const& { return *value_; }
  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }
  [[nodiscard]] const T* operator->() const { return &*value_; }
  [[nodiscard]] T* operator->() { return &*value_; }
  [[nodiscard]] const T& operator*() const& { return *value_; }
  [[nodiscard]] T& operator*() & { return *value_; }

  template <class U>
  [[nodiscard]] T value_or(U&& fallback) const {
    return value_.has_value() ? *value_ : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  std::optional<T> value_;
  Status status_;
};

/// Convenience for functions that only need to report success or failure.
using VoidResult = Status;

}  // namespace itf

#endif  // ITF_ERROR_HPP
