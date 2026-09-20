// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/codec.hpp"

#include <cstring>

namespace itf {
namespace {

void store_u16(std::vector<std::uint8_t>& buffer, std::uint16_t value) {
  buffer.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  buffer.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void store_u32(std::vector<std::uint8_t>& buffer, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

void store_u64(std::vector<std::uint8_t>& buffer, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    buffer.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
  }
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  const std::size_t size = text.size();
  std::size_t index = 0;
  while (index < size) {
    const unsigned char lead = bytes[index];
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if (lead < 0x80U) {
      ++index;
      continue;
    }
    if ((lead & 0xE0U) == 0xC0U) {
      extra = 1;
      code_point = static_cast<std::uint32_t>(lead & 0x1FU);
    } else if ((lead & 0xF0U) == 0xE0U) {
      extra = 2;
      code_point = static_cast<std::uint32_t>(lead & 0x0FU);
    } else if ((lead & 0xF8U) == 0xF0U) {
      extra = 3;
      code_point = static_cast<std::uint32_t>(lead & 0x07U);
    } else {
      return false;
    }
    if (index + extra >= size) return false;
    for (std::size_t offset = 1; offset <= extra; ++offset) {
      const unsigned char continuation = bytes[index + offset];
      if ((continuation & 0xC0U) != 0x80U) return false;
      code_point = (code_point << 6U) | static_cast<std::uint32_t>(continuation & 0x3FU);
    }
    // Reject overlong encodings, surrogates and out-of-range code points.
    if (extra == 1 && code_point < 0x80U) return false;
    if (extra == 2 && code_point < 0x800U) return false;
    if (extra == 3 && code_point < 0x10000U) return false;
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) return false;
    if (code_point > 0x10FFFFU) return false;
    index += extra + 1;
  }
  return true;
}

bool is_safe_label(std::string_view text) noexcept {
  if (text.empty() || text.size() > 128) return false;
  for (const char raw : text) {
    const unsigned char value = static_cast<unsigned char>(raw);
    const bool alphanumeric = (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
                              (value >= 'A' && value <= 'Z');
    const bool punctuation = value == '.' || value == '_' || value == '-' || value == ':';
    if (!alphanumeric && !punctuation) return false;
  }
  return true;
}

bool Writer::reserve_field(std::size_t count) {
  if (fail_ != StatusCode::Ok) return false;
  if (count > limit_ || buffer_.size() > limit_ - count) {
    fail(StatusCode::BoundsExceeded);
    return false;
  }
  return true;
}

Writer& Writer::u8(std::uint8_t value) {
  if (!reserve_field(1)) return *this;
  buffer_.push_back(value);
  return *this;
}

Writer& Writer::u16(std::uint16_t value) {
  if (!reserve_field(2)) return *this;
  store_u16(buffer_, value);
  return *this;
}

Writer& Writer::u32(std::uint32_t value) {
  if (!reserve_field(4)) return *this;
  store_u32(buffer_, value);
  return *this;
}

Writer& Writer::u64(std::uint64_t value) {
  if (!reserve_field(8)) return *this;
  store_u64(buffer_, value);
  return *this;
}

Writer& Writer::i64(std::int64_t value) {
  return u64(static_cast<std::uint64_t>(value));
}

Writer& Writer::boolean(bool value) {
  return u8(value ? 1U : 0U);
}

Writer& Writer::raw(ByteSpan bytes) {
  if (!reserve_field(bytes.size())) return *this;
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  return *this;
}

Writer& Writer::bytes_field(ByteSpan bytes) {
  if (bytes.size() > kMaxFramePayloadBytes) {
    fail(StatusCode::BoundsExceeded);
    return *this;
  }
  u32(static_cast<std::uint32_t>(bytes.size()));
  return raw(bytes);
}

Writer& Writer::string_field(std::string_view text) {
  if (text.size() > kMaxFieldStringBytes) {
    fail(StatusCode::BoundsExceeded);
    return *this;
  }
  if (!is_valid_utf8(text)) {
    fail(StatusCode::MalformedInput);
    return *this;
  }
  return bytes_field(as_bytes(text));
}

bool Reader::require(std::size_t count) {
  if (fail_ != StatusCode::Ok) return false;
  if (count > data_.size() || offset_ > data_.size() - count) {
    fail(StatusCode::MalformedInput);
    return false;
  }
  return true;
}

std::uint8_t Reader::u8() {
  if (!require(1)) return 0;
  const std::uint8_t value = data_[offset_];
  offset_ += 1;
  return value;
}

std::uint16_t Reader::u16() {
  if (!require(2)) return 0;
  const std::uint16_t value = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(data_[offset_]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8U));
  offset_ += 2;
  return value;
}

std::uint32_t Reader::u32() {
  if (!require(4)) return 0;
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(index)])
             << static_cast<unsigned>(index * 8);
  }
  offset_ += 4;
  return value;
}

std::uint64_t Reader::u64() {
  if (!require(8)) return 0;
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(index)])
             << static_cast<unsigned>(index * 8);
  }
  offset_ += 8;
  return value;
}

std::int64_t Reader::i64() {
  return static_cast<std::int64_t>(u64());
}

bool Reader::boolean() {
  const std::uint8_t value = u8();
  if (fail_ != StatusCode::Ok) return false;
  if (value > 1U) {
    fail(StatusCode::MalformedInput);
    return false;
  }
  return value == 1U;
}

std::uint32_t Reader::count(std::size_t max_entries) {
  const std::uint32_t value = u32();
  if (fail_ != StatusCode::Ok) return 0;
  if (static_cast<std::size_t>(value) > max_entries) {
    fail(StatusCode::BoundsExceeded);
    return 0;
  }
  return value;
}

ByteSpan Reader::bytes_field() {
  const std::uint32_t length = u32();
  if (fail_ != StatusCode::Ok) return ByteSpan();
  return raw(static_cast<std::size_t>(length));
}

std::string_view Reader::string_field() {
  const ByteSpan bytes = bytes_field();
  if (fail_ != StatusCode::Ok) return std::string_view();
  if (bytes.size() > kMaxFieldStringBytes) {
    fail(StatusCode::BoundsExceeded);
    return std::string_view();
  }
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!is_valid_utf8(text)) {
    fail(StatusCode::MalformedInput);
    return std::string_view();
  }
  return text;
}

std::string Reader::string_field_owned() { return std::string(string_field()); }

ByteSpan Reader::raw(std::size_t count) {
  if (!require(count)) return ByteSpan();
  if (count > limit_) {
    fail(StatusCode::BoundsExceeded);
    return ByteSpan();
  }
  const ByteSpan span(data_.data() + offset_, count);
  offset_ += count;
  return span;
}

Status Reader::finish() const {
  if (fail_ != StatusCode::Ok) {
    return Status(fail_, "reader failed before completion");
  }
  if (offset_ != data_.size()) {
    return Status(StatusCode::TrailingGarbage, "unexpected trailing bytes after message body");
  }
  return Status::success();
}

}  // namespace itf
