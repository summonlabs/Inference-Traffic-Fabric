// Inference Traffic Fabric - canonical bounded binary encoding.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_CODEC_HPP
#define ITF_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "itf/bytes.hpp"
#include "itf/error.hpp"
#include "itf/ids.hpp"

namespace itf {

/// Hard limits applied by every decoder in this library. Externally supplied
/// sizes never reach an allocation without passing one of these bounds.
inline constexpr std::size_t kMaxFieldStringBytes = 4096;
inline constexpr std::size_t kMaxFramePayloadBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxSnapshotPayloadBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kMaxCollectionEntries = 65536U;

/// True when the byte range is well-formed UTF-8 (no overlong forms, no
/// surrogates, no truncated sequences, no code points above U+10FFFF).
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// True when the text is a bounded, non-empty label made of characters that
/// are safe to round-trip through logs and CLIs.
[[nodiscard]] bool is_safe_label(std::string_view text) noexcept;

class Writer {
 public:
  explicit Writer(std::size_t limit = kMaxFramePayloadBytes) : limit_(limit) {}

  Writer& u8(std::uint8_t value);
  Writer& u16(std::uint16_t value);
  Writer& u32(std::uint32_t value);
  Writer& u64(std::uint64_t value);
  Writer& i64(std::int64_t value);
  Writer& boolean(bool value);
  Writer& raw(ByteSpan bytes);
  Writer& bytes_field(ByteSpan bytes);
  Writer& string_field(std::string_view text);

  template <class Tag>
  Writer& id(const Id128<Tag>& value) {
    u64(value.high());
    u64(value.low());
    return *this;
  }

  template <class Tag>
  Writer& generation(const Generation<Tag>& value) {
    u64(value.value());
    return *this;
  }

  template <class E>
  Writer& enumeration(E value) {
    u16(static_cast<std::uint16_t>(value));
    return *this;
  }

  [[nodiscard]] bool ok() const noexcept { return fail_ == StatusCode::Ok; }
  [[nodiscard]] StatusCode fail_code() const noexcept { return fail_; }
  [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(buffer_); }

 private:
  bool reserve_field(std::size_t count);
  void fail(StatusCode code) noexcept {
    if (fail_ == StatusCode::Ok) fail_ = code;
  }

  std::vector<std::uint8_t> buffer_;
  std::size_t limit_;
  StatusCode fail_ = StatusCode::Ok;
};

class Reader {
 public:
  explicit Reader(ByteSpan data, std::size_t limit = kMaxFramePayloadBytes)
      : data_(data), limit_(limit) {
    if (data_.size() > limit_) fail(StatusCode::BoundsExceeded);
  }

  [[nodiscard]] bool ok() const noexcept { return fail_ == StatusCode::Ok; }
  [[nodiscard]] StatusCode fail_code() const noexcept { return fail_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return ok() ? data_.size() - offset_ : 0; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::int64_t i64();
  bool boolean();

  /// Bounded collection count. Refuses counts above max_entries so that a
  /// hostile length prefix can never drive an unbounded allocation.
  std::uint32_t count(std::size_t max_entries = kMaxCollectionEntries);

  ByteSpan bytes_field();
  std::string_view string_field();
  [[nodiscard]] std::string string_field_owned();
  ByteSpan raw(std::size_t count);

  template <class E>
  E enumeration(E max_exclusive) {
    const std::uint16_t raw_value = u16();
    if (fail_ != StatusCode::Ok) return static_cast<E>(0);
    if (raw_value >= static_cast<std::uint16_t>(max_exclusive)) {
      fail(StatusCode::MalformedInput);
      return static_cast<E>(0);
    }
    return static_cast<E>(raw_value);
  }

  template <class Tag>
  Id128<Tag> id() {
    const std::uint64_t high = u64();
    const std::uint64_t low = u64();
    if (fail_ != StatusCode::Ok) return Id128<Tag>();
    if (high == 0 && low == 0) fail(StatusCode::MalformedInput);
    return Id128<Tag>(high, low);
  }

  template <class Tag>
  Generation<Tag> generation() {
    const std::uint64_t value = u64();
    if (fail_ != StatusCode::Ok) return Generation<Tag>();
    if (value == 0) fail(StatusCode::MalformedInput);
    return Generation<Tag>::from(value);
  }

  /// Rejects trailing garbage. Call after decoding a complete message.
  [[nodiscard]] Status finish() const;

  void fail(StatusCode code) noexcept {
    if (fail_ == StatusCode::Ok) fail_ = code;
  }

 private:
  bool require(std::size_t count);

  ByteSpan data_;
  std::size_t offset_ = 0;
  std::size_t limit_;
  StatusCode fail_ = StatusCode::Ok;
};

}  // namespace itf

#endif  // ITF_CODEC_HPP
