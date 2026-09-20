// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/error.hpp"

namespace itf {

std::string_view to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "OK";
    case StatusCode::InvalidArgument: return "INVALID_ARGUMENT";
    case StatusCode::OutOfRange: return "OUT_OF_RANGE";
    case StatusCode::NotFound: return "NOT_FOUND";
    case StatusCode::AlreadyExists: return "ALREADY_EXISTS";
    case StatusCode::BoundsExceeded: return "BOUNDS_EXCEEDED";
    case StatusCode::IntegrityMismatch: return "INTEGRITY_MISMATCH";
    case StatusCode::MalformedInput: return "MALFORMED_INPUT";
    case StatusCode::TrailingGarbage: return "TRAILING_GARBAGE";
    case StatusCode::UnsupportedVersion: return "UNSUPPORTED_VERSION";
    case StatusCode::Unsupported: return "UNSUPPORTED";
    case StatusCode::StaleGeneration: return "STALE_GENERATION";
    case StatusCode::StaleEpoch: return "STALE_EPOCH";
    case StatusCode::StaleAuthority: return "STALE_AUTHORITY";
    case StatusCode::Cancelled: return "CANCELLED";
    case StatusCode::Conflict: return "CONFLICT";
    case StatusCode::NotAuthorized: return "NOT_AUTHORIZED";
    case StatusCode::CapacityExhausted: return "CAPACITY_EXHAUSTED";
    case StatusCode::InvalidTransition: return "INVALID_TRANSITION";
    case StatusCode::PolicyInvalid: return "POLICY_INVALID";
    case StatusCode::EvidenceUnknown: return "EVIDENCE_UNKNOWN";
    case StatusCode::EvidenceStale: return "EVIDENCE_STALE";
    case StatusCode::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case StatusCode::ReplayRejected: return "REPLAY_REJECTED";
    case StatusCode::Closed: return "CLOSED";
    case StatusCode::ShuttingDown: return "SHUTTING_DOWN";
    case StatusCode::IoError: return "IO_ERROR";
    case StatusCode::Internal: return "INTERNAL";
    case StatusCode::DuplicateIgnored: return "DUPLICATE_IGNORED";
    case StatusCode::Refused: return "REFUSED";
  }
  return "UNKNOWN_STATUS";
}

std::string Status::to_string() const {
  std::string out;
  out.reserve(32 + message_.size());
  out.append(itf::to_string(code_));
  if (!message_.empty()) {
    out.append(": ");
    out.append(message_);
  }
  return out;
}

}  // namespace itf
