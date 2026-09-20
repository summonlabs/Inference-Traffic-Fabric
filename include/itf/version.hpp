// Inference Traffic Fabric - version and compatibility constants.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_VERSION_HPP
#define ITF_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace itf {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";

/// Version of the public C++ API surface. Bumped on incompatible API changes.
inline constexpr std::uint32_t kApiVersion = 1;

/// Version of the framed wire protocol. Peers must match exactly.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

/// Version of the durable snapshot format. Older formats load only through an
/// explicit migration, never implicitly.
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;

inline constexpr std::string_view kCopyrightNotice = "Copyright 2026 Summon Software Labs.";

}  // namespace itf

#endif  // ITF_VERSION_HPP
