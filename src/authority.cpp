// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/authority.hpp"

namespace itf {

std::string AuthorityStamp::to_string() const {
  std::string out;
  out.reserve(96);
  out.append("epoch=");
  out.append(std::to_string(epoch.value()));
  out.append(" incarnation=");
  out.append(incarnation.to_string());
  out.append(" policy=");
  out.append(std::to_string(policy.value()));
  out.append(" sequence=");
  out.append(std::to_string(sequence.value()));
  return out;
}

std::string_view to_string(AuthorityVerdict value) noexcept {
  switch (value) {
    case AuthorityVerdict::Unknown: return "UNKNOWN";
    case AuthorityVerdict::Current: return "CURRENT";
    case AuthorityVerdict::StaleEpoch: return "STALE_EPOCH";
    case AuthorityVerdict::StalePolicy: return "STALE_POLICY";
    case AuthorityVerdict::StaleIncarnation: return "STALE_INCARNATION";
    case AuthorityVerdict::Absent: return "ABSENT";
    case AuthorityVerdict::Count: return "COUNT";
  }
  return "UNKNOWN";
}

AuthorityVerdict validate_authority(const AuthorityStamp& stamp, CoordinatorEpoch current_epoch,
                                    BootId current_incarnation,
                                    PolicyGeneration current_policy) noexcept {
  if (stamp.epoch.is_unset() || stamp.incarnation.is_nil()) return AuthorityVerdict::Absent;
  if (stamp.epoch != current_epoch) return AuthorityVerdict::StaleEpoch;
  if (stamp.incarnation != current_incarnation) return AuthorityVerdict::StaleIncarnation;
  if (stamp.policy != current_policy) return AuthorityVerdict::StalePolicy;
  if (stamp.sequence.is_unset()) return AuthorityVerdict::Absent;
  return AuthorityVerdict::Current;
}

}  // namespace itf
