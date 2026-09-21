// Degraded Mode Fabric - core primitives implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/core.hpp"

#include <algorithm>
#include <cstdio>

namespace dmf {

std::uint32_t crc32c(const std::uint8_t* data, std::size_t size, std::uint32_t seed) noexcept {
  std::uint32_t crc = ~seed;
  for (std::size_t i = 0; i < size; ++i) {
    crc = detail::kCrc32cTable[static_cast<std::size_t>((crc ^ data[i]) & 0xFFU)] ^ (crc >> 8);
  }
  return ~crc;
}

std::uint32_t crc32c(std::string_view data, std::uint32_t seed) noexcept {
  return crc32c(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), seed);
}

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size, std::uint64_t seed) noexcept {
  std::uint64_t hash = seed;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<std::uint64_t>(data[i]);
    hash *= detail::kFnvPrime;
  }
  return hash;
}

void Digest64::update(const void* data, std::size_t size) noexcept {
  state_ = fnv1a64(static_cast<const std::uint8_t*>(data), size, state_);
}

void Digest64::update(std::string_view text) noexcept {
  update(text.data(), text.size());
}

void Digest64::update_u8(std::uint8_t value) noexcept { update(&value, sizeof(value)); }

void Digest64::update_u16(std::uint16_t value) noexcept {
  const std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xFFU),
                                 static_cast<std::uint8_t>((value >> 8) & 0xFFU)};
  update(bytes, sizeof(bytes));
}

void Digest64::update_u32(std::uint32_t value) noexcept {
  const std::uint8_t bytes[4] = {
      static_cast<std::uint8_t>(value & 0xFFU), static_cast<std::uint8_t>((value >> 8) & 0xFFU),
      static_cast<std::uint8_t>((value >> 16) & 0xFFU),
      static_cast<std::uint8_t>((value >> 24) & 0xFFU)};
  update(bytes, sizeof(bytes));
}

void Digest64::update_u64(std::uint64_t value) noexcept {
  std::uint8_t bytes[8] = {};
  for (int i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
  update(bytes, sizeof(bytes));
}

void Digest64::update_bool(bool value) noexcept { update_u8(value ? std::uint8_t{1} : std::uint8_t{0}); }

bool is_valid(ErrorCode code) noexcept {
  return static_cast<std::uint16_t>(code) <= static_cast<std::uint16_t>(ErrorCode::QuotaExceeded);
}

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::OutOfRange: return "OUT_OF_RANGE";
    case ErrorCode::CapacityExceeded: return "CAPACITY_EXCEEDED";
    case ErrorCode::Overflow: return "OVERFLOW";
    case ErrorCode::NotFound: return "NOT_FOUND";
    case ErrorCode::AlreadyExists: return "ALREADY_EXISTS";
    case ErrorCode::InvalidState: return "INVALID_STATE";
    case ErrorCode::Unsupported: return "UNSUPPORTED";
    case ErrorCode::Unknown: return "UNKNOWN";
    case ErrorCode::Stale: return "STALE";
    case ErrorCode::Conflict: return "CONFLICT";
    case ErrorCode::Invalid: return "INVALID";
    case ErrorCode::Corrupt: return "CORRUPT";
    case ErrorCode::Truncated: return "TRUNCATED";
    case ErrorCode::VersionUnsupported: return "VERSION_UNSUPPORTED";
    case ErrorCode::IntegrityFailure: return "INTEGRITY_FAILURE";
    case ErrorCode::SequenceRegression: return "SEQUENCE_REGRESSION";
    case ErrorCode::ReplayDetected: return "REPLAY_DETECTED";
    case ErrorCode::Unauthorized: return "UNAUTHORIZED";
    case ErrorCode::InvalidEnum: return "INVALID_ENUM";
    case ErrorCode::TrailingGarbage: return "TRAILING_GARBAGE";
    case ErrorCode::SearchLimitReached: return "SEARCH_LIMIT_REACHED";
    case ErrorCode::Indeterminate: return "INDETERMINATE";
    case ErrorCode::Refused: return "REFUSED";
    case ErrorCode::IoError: return "IO_ERROR";
    case ErrorCode::Shutdown: return "SHUTDOWN";
    case ErrorCode::Exhausted: return "EXHAUSTED";
    case ErrorCode::NotSupportedByHost: return "NOT_SUPPORTED_BY_HOST";
    case ErrorCode::Underflow: return "UNDERFLOW";
    case ErrorCode::QuotaExceeded: return "QUOTA_EXCEEDED";
  }
  return "UNRECOGNISED";
}

Status::Status(ErrorCode code, std::string_view detail) noexcept : code_(code) {
  detail_ = bounded_detail(detail);
}

std::string Status::render() const {
  std::string out(to_string(code_));
  if (!detail_.empty()) {
    out.append(": ");
    out.append(detail_);
  }
  return out;
}

bool ByteWriter::ensure(std::size_t count) noexcept {
  if (!ok_) return false;
  const auto next = checked_add(data_.size(), count);
  if (!next.has_value() || *next > max_total_) {
    ok_ = false;
    return false;
  }
  return true;
}

void ByteWriter::u8(std::uint8_t value) {
  if (!ensure(1)) return;
  data_.push_back(value);
}

void ByteWriter::u16(std::uint16_t value) {
  if (!ensure(2)) return;
  data_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void ByteWriter::u32(std::uint32_t value) {
  if (!ensure(4)) return;
  for (int i = 0; i < 4; ++i) {
    data_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  if (!ensure(8)) return;
  for (int i = 0; i < 8; ++i) {
    data_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
  }
}

void ByteWriter::boolean(bool value) { u8(value ? std::uint8_t{1} : std::uint8_t{0}); }

void ByteWriter::raw(const void* data, std::size_t size) {
  if (!ensure(size)) return;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  data_.insert(data_.end(), bytes, bytes + size);
}

void ByteWriter::blob(std::string_view text) {
  if (text.size() > max_blob_) {
    ok_ = false;
    return;
  }
  if (!ensure(text.size() + 4)) return;
  u32(static_cast<std::uint32_t>(text.size()));
  raw(text.data(), text.size());
}

void ByteWriter::padding(std::size_t count) {
  if (!ensure(count)) return;
  data_.insert(data_.end(), count, std::uint8_t{0});
}

void ByteReader::fail(ErrorCode code) noexcept {
  if (error_ == ErrorCode::Ok) error_ = code;
}

void ByteReader::poison(ErrorCode code) noexcept { fail(code); }

std::string_view ByteReader::raw(std::size_t count) {
  if (!ok()) return {};
  if (count > remaining()) {
    fail(ErrorCode::Truncated);
    return {};
  }
  const auto* start = reinterpret_cast<const char*>(data_ + position_);
  position_ += count;
  return std::string_view(start, count);
}

std::uint8_t ByteReader::u8() {
  if (!ok() || remaining() < 1) {
    fail(ErrorCode::Truncated);
    return 0;
  }
  return data_[position_++];
}

std::uint16_t ByteReader::u16() {
  if (!ok() || remaining() < 2) {
    fail(ErrorCode::Truncated);
    return 0;
  }
  const std::uint16_t value = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(data_[position_]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[position_ + 1]) << 8));
  position_ += 2;
  return value;
}

std::uint32_t ByteReader::u32() {
  if (!ok() || remaining() < 4) {
    fail(ErrorCode::Truncated);
    return 0;
  }
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(data_[position_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  position_ += 4;
  return value;
}

std::uint64_t ByteReader::u64() {
  if (!ok() || remaining() < 8) {
    fail(ErrorCode::Truncated);
    return 0;
  }
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data_[position_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  position_ += 8;
  return value;
}

bool ByteReader::boolean() {
  const std::uint8_t raw_value = u8();
  if (!ok()) return false;
  if (raw_value > 1) {
    fail(ErrorCode::InvalidEnum);
    return false;
  }
  return raw_value == 1;
}

std::string_view ByteReader::blob() {
  if (!ok()) return {};
  const std::uint32_t length = u32();
  if (!ok()) return {};
  if (length > max_blob_) {
    fail(ErrorCode::CapacityExceeded);
    return {};
  }
  return raw(length);
}

std::uint32_t ByteReader::count(std::uint32_t max_allowed) noexcept {
  if (!ok()) return 0;
  const std::uint32_t value = u32();
  if (!ok()) return 0;
  if (value > max_allowed) {
    fail(ErrorCode::CapacityExceeded);
    return 0;
  }
  return value;
}

std::string to_hex(std::uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
  return std::string(buffer);
}

std::string to_hex(std::uint32_t value) {
  char buffer[9];
  std::snprintf(buffer, sizeof(buffer), "%08lx", static_cast<unsigned long>(value));
  return std::string(buffer);
}

bool is_text_safe(std::string_view text) noexcept {
  for (const char raw : text) {
    const auto ch = static_cast<unsigned char>(raw);
    if (ch == 0) return false;
    if (ch < 0x20U && ch != 0x09U) return false;
    if (ch == 0x7FU) return false;
  }
  return true;
}

std::string bounded_detail(std::string_view text, std::size_t limit) {
  std::string out;
  out.reserve(std::min(text.size(), limit));
  for (const char raw : text) {
    if (out.size() >= limit) break;
    const auto ch = static_cast<unsigned char>(raw);
    if (ch == 0 || (ch < 0x20U && ch != 0x09U) || ch == 0x7FU) {
      out.push_back('?');
    } else {
      out.push_back(raw);
    }
  }
  return out;
}

}  // namespace dmf
