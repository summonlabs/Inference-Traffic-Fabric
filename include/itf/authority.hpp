// Inference Traffic Fabric - authority stamps and generation fencing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_AUTHORITY_HPP
#define ITF_AUTHORITY_HPP

#include <string>

#include "itf/ids.hpp"
#include "itf/traffic.hpp"

namespace itf {

/// Identity of one coordinator incarnation. A restart advances the epoch and
/// issues a fresh boot identity, so authority granted by a previous
/// incarnation can never be mistaken for current authority.
struct AuthorityStamp {
  CoordinatorEpoch epoch;
  BootId incarnation;
  PolicyGeneration policy;
  AuthoritySequence sequence;

  [[nodiscard]] bool is_valid() const noexcept {
    return !epoch.is_unset() && !incarnation.is_nil() && !policy.is_unset() && !sequence.is_unset();
  }

  [[nodiscard]] bool same_incarnation(const AuthorityStamp& other) const noexcept {
    return epoch == other.epoch && incarnation == other.incarnation;
  }

  [[nodiscard]] std::string to_string() const;
};

/// Result of validating a caller-supplied stamp against current authority.
enum class AuthorityVerdict : std::uint16_t {
  Unknown = 0,
  Current = 1,
  StaleEpoch = 2,
  StalePolicy = 3,
  StaleIncarnation = 4,
  Absent = 5,
  Count = 6,
};

[[nodiscard]] std::string_view to_string(AuthorityVerdict value) noexcept;

/// Validates a previously issued stamp. An absent epoch is treated as Absent,
/// never as Current: missing evidence never becomes positive evidence.
[[nodiscard]] AuthorityVerdict validate_authority(const AuthorityStamp& stamp,
                                                  CoordinatorEpoch current_epoch,
                                                  BootId current_incarnation,
                                                  PolicyGeneration current_policy) noexcept;

}  // namespace itf

#endif  // ITF_AUTHORITY_HPP
