// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <cstdint>
#include <string>

#include "itf/bytes.hpp"
#include "itf/codec.hpp"
#include "itf/crc32c.hpp"
#include "itf/error.hpp"
#include "itf/ids.hpp"
#include "itf/time.hpp"
#include "itf/traffic.hpp"
#include "support/test.hpp"

namespace {

using namespace itf;

ITF_TEST(identities_round_trip) {
  const RequestId id(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL);
  ITF_CHECK_EQ(id.to_string(), std::string("0123456789abcdeffedcba9876543210"));
  const auto parsed = RequestId::parse(id.to_string());
  ITF_REQUIRE(parsed.has_value());
  ITF_CHECK(*parsed == id);
  ITF_CHECK(RequestId().is_nil());
  ITF_CHECK(!id.is_nil());
}

ITF_TEST(identity_parsing_is_strict) {
  const std::string valid = "0123456789abcdeffedcba9876543210";
  ITF_CHECK(RequestId::parse(valid).has_value());
  ITF_CHECK(!RequestId::parse("").has_value());
  ITF_CHECK(!RequestId::parse("0123456789abcdeffedcba987654321").has_value());
  ITF_CHECK(!RequestId::parse("0123456789abcdeffedcba98765432100").has_value());
  ITF_CHECK(!RequestId::parse("0123456789abcdeffedcba987654321g").has_value());
  ITF_CHECK(!RequestId::parse(" 123456789abcdeffedcba9876543210").has_value());
  ITF_CHECK(!RequestId::parse("0x123456789abcdeffedcba98765432").has_value());
}

ITF_TEST(identity_hash_distinguishes_halves) {
  const RequestId first(0x1ULL, 0x2ULL);
  const RequestId second(0x2ULL, 0x1ULL);
  ITF_CHECK(std::hash<RequestId>{}(first) != std::hash<RequestId>{}(second));
}

ITF_TEST(generation_semantics) {
  const ModelGeneration unset;
  ITF_CHECK(unset.is_unset());
  const ModelGeneration first = ModelGeneration::from(1);
  ITF_CHECK(!first.is_unset());
  ITF_CHECK_EQ(first.next().value(), 2ULL);
  ITF_CHECK(first < first.next());
  ITF_CHECK(first != ModelGeneration::from(2));
}

ITF_TEST(crc32c_known_vectors) {
  const std::string check = "123456789";
  ITF_CHECK_EQ(Crc32c::compute(as_bytes(check)), 0xE3069283U);
  ITF_CHECK_EQ(Crc32c::compute(ByteSpan()), 0U);
  Crc32c incremental;
  incremental.update(as_bytes(std::string_view("1234")));
  incremental.update(as_bytes(std::string_view("56789")));
  ITF_CHECK_EQ(incremental.value(), 0xE3069283U);
}

ITF_TEST(checked_arithmetic_refuses_overflow) {
  std::uint64_t out = 0;
  ITF_CHECK(checked_add(1, 2, out));
  ITF_CHECK_EQ(out, 3ULL);
  ITF_CHECK(!checked_add(UINT64_MAX, 1, out));
  ITF_CHECK(checked_mul(4, 5, out));
  ITF_CHECK_EQ(out, 20ULL);
  ITF_CHECK(!checked_mul(UINT64_MAX, 2, out));
}

ITF_TEST(codec_round_trip) {
  Writer writer;
  writer.u8(7).u16(0x1234).u32(0xDEADBEEF).u64(0x0123456789ABCDEFULL).i64(-42).boolean(true);
  writer.string_field("peer-1");
  writer.bytes_field(as_bytes(std::string_view("payload")));
  writer.id(RequestId(1, 2));
  writer.generation(ModelGeneration::from(9));
  ITF_REQUIRE(writer.ok());

  Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
  ITF_CHECK_EQ(reader.u8(), static_cast<std::uint8_t>(7));
  ITF_CHECK_EQ(reader.u16(), static_cast<std::uint16_t>(0x1234));
  ITF_CHECK_EQ(reader.u32(), 0xDEADBEEFU);
  ITF_CHECK_EQ(reader.u64(), 0x0123456789ABCDEFULL);
  ITF_CHECK_EQ(reader.i64(), static_cast<std::int64_t>(-42));
  ITF_CHECK(reader.boolean());
  ITF_CHECK_EQ(std::string(reader.string_field()), std::string("peer-1"));
  ITF_CHECK_EQ(reader.bytes_field().size(), static_cast<std::size_t>(7));
  ITF_CHECK(reader.id<RequestIdTag>() == RequestId(1, 2));
  ITF_CHECK_EQ(reader.generation<ModelGenerationTag>().value(), 9ULL);
  ITF_REQUIRE_STATUS_OK(reader.finish());
}

ITF_TEST(codec_detects_truncation_and_trailing_bytes) {
  Writer writer;
  writer.u32(1).u32(2);
  ITF_REQUIRE(writer.ok());
  {
    Reader reader(ByteSpan(writer.buffer().data(), 3));
    (void)reader.u32();
    ITF_CHECK(!reader.ok());
    ITF_CHECK_EQ(reader.fail_code(), StatusCode::MalformedInput);
  }
  {
    Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
    (void)reader.u32();
    ITF_CHECK_EQ(reader.finish().code(), StatusCode::TrailingGarbage);
  }
}

ITF_TEST(codec_enforces_field_bounds) {
  const std::string oversized(kMaxFieldStringBytes + 1, 'a');
  Writer writer;
  writer.string_field(oversized);
  ITF_CHECK(!writer.ok());
  ITF_CHECK_EQ(writer.fail_code(), StatusCode::BoundsExceeded);

  std::array<std::uint8_t, 4> hostile = {0xFF, 0xFF, 0xFF, 0xFF};
  Reader reader(ByteSpan(hostile.data(), hostile.size()));
  ITF_CHECK_EQ(reader.count(16), 0U);
  ITF_CHECK(!reader.ok());
  ITF_CHECK_EQ(reader.fail_code(), StatusCode::BoundsExceeded);
}

ITF_TEST(codec_rejects_nil_identities_and_zero_generations) {
  std::array<std::uint8_t, 16> zeros{};
  zeros.fill(0);
  Reader reader(ByteSpan(zeros.data(), zeros.size()));
  (void)reader.id<RequestIdTag>();
  ITF_CHECK(!reader.ok());
  Reader generation_reader(ByteSpan(zeros.data(), zeros.size()));
  (void)generation_reader.generation<ModelGenerationTag>();
  ITF_CHECK(!generation_reader.ok());
}

ITF_TEST(codec_rejects_unknown_enum_values) {
  const std::array<std::uint8_t, 2> unknown = {0xFF, 0x00};
  Reader reader(ByteSpan(unknown.data(), unknown.size()));
  (void)reader.enumeration(TrafficClass::Count);
  ITF_CHECK(!reader.ok());
  ITF_CHECK_EQ(reader.fail_code(), StatusCode::MalformedInput);
}

ITF_TEST(utf8_validation) {
  ITF_CHECK(is_valid_utf8("plain ascii"));
  ITF_CHECK(is_valid_utf8("\xC3\xA9"));                 // e-acute
  ITF_CHECK(is_valid_utf8("\xE2\x82\xAC"));             // euro sign
  ITF_CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));         // emoji
  ITF_CHECK(!is_valid_utf8("\xC3"));                    // truncated
  ITF_CHECK(!is_valid_utf8("\xC0\xAF"));                // overlong
  ITF_CHECK(!is_valid_utf8("\xED\xA0\x80"));            // surrogate
  ITF_CHECK(!is_valid_utf8("\xF5\x80\x80\x80"));        // above U+10FFFF
  ITF_CHECK(!is_valid_utf8("\x80"));                    // stray continuation
}

ITF_TEST(utf8_validation_does_not_read_past_the_end) {
  // A truncated multi-byte sequence at the very end of the buffer must be
  // refused without touching memory beyond it.
  ITF_CHECK(!is_valid_utf8("abc\xF0\x9F\x98"));
  ITF_CHECK(!is_valid_utf8("\xF0"));
}

ITF_TEST(safe_labels) {
  ITF_CHECK(is_safe_label("prefill-1"));
  ITF_CHECK(is_safe_label("node.a:1"));
  ITF_CHECK(!is_safe_label(""));
  ITF_CHECK(!is_safe_label("has space"));
  ITF_CHECK(!is_safe_label(std::string(129, 'a')));
  ITF_CHECK(!is_safe_label("slash/name"));
}

ITF_TEST(instant_semantics) {
  const Instant unset;
  ITF_CHECK(!unset.is_set());
  ITF_CHECK(!unset.expired_at(1000));
  ITF_CHECK_EQ(unset.remaining_at(0), static_cast<std::int64_t>(0));
  const Instant deadline = Instant::at(1000);
  ITF_CHECK(deadline.is_set());
  ITF_CHECK(!deadline.expired_at(999));
  ITF_CHECK(deadline.expired_at(1000));
  ITF_CHECK_EQ(deadline.remaining_at(400), static_cast<std::int64_t>(600));
  // An unset instant sorts after every set instant, so it never looks expired.
  ITF_CHECK(deadline < unset);
  ITF_CHECK(!(unset < deadline));
}

ITF_TEST(virtual_clock_is_deterministic) {
  VirtualClock clock(100);
  ITF_CHECK_EQ(clock.now_nanos(), static_cast<std::int64_t>(100));
  clock.advance(50);
  ITF_CHECK_EQ(clock.now_nanos(), static_cast<std::int64_t>(150));
  clock.advance(-10);
  ITF_CHECK_EQ(clock.now_nanos(), static_cast<std::int64_t>(150));
  clock.set(7);
  ITF_CHECK_EQ(clock.now_nanos(), static_cast<std::int64_t>(7));
}

ITF_TEST(status_codes_are_stable_text) {
  ITF_CHECK_EQ(std::string(to_string(StatusCode::Ok)), std::string("OK"));
  ITF_CHECK_EQ(std::string(to_string(StatusCode::RevalidationRequired)),
               std::string("REVALIDATION_REQUIRED"));
  const Status status(StatusCode::Conflict, "detail");
  ITF_CHECK(!status.ok());
  ITF_CHECK_EQ(status.to_string(), std::string("CONFLICT: detail"));
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_foundation", argc, argv); }
