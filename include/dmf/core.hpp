// Degraded Mode Fabric - portable core primitives.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef DMF_CORE_HPP
#define DMF_CORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "dmf/version.hpp"

namespace dmf {

// ---------------------------------------------------------------------------
// Global hard bounds. Every externally reachable collection is checked against
// one of these before allocation so that a hostile or corrupt peer cannot make
// the runtime materialise unbounded state.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxBlobBytes = 1U << 20;          // 1 MiB
inline constexpr std::size_t kMaxDocumentBytes = 16U << 20;     // 16 MiB
inline constexpr std::uint32_t kMaxCollectionItems = 1U << 20;  // 1,048,576
inline constexpr std::size_t kMaxDetailBytes = 192;
inline constexpr std::size_t kMaxExplanationBytes = 8U << 10;
inline constexpr std::size_t kMaxPathBytes = 512;

// Per-collection bounds. A declared count above its bound fails before any
// allocation happens, on every decode path.
inline constexpr std::uint32_t kMaxGuaranteesPerSet = 16;
inline constexpr std::uint32_t kMaxEnvelopeBounds = 16;
inline constexpr std::uint32_t kMaxDeltaEntries = 32;
inline constexpr std::uint32_t kMaxEvidenceItems = 1U << 16;
inline constexpr std::uint32_t kMaxReasonsPerDecision = 32;
inline constexpr std::uint32_t kMaxSupportEntries = 16;
inline constexpr std::uint32_t kMaxPreconditions = 32;
inline constexpr std::uint32_t kMaxPreconditionEvaluations = 32;
inline constexpr std::uint32_t kMaxPlanEntries = 1U << 20;
inline constexpr std::uint32_t kMaxRulesPerPolicy = 4096;
inline constexpr std::uint32_t kMaxClassProfiles = 8;

// ---------------------------------------------------------------------------
// Integrity primitives.
//
// These are *integrity* checks, not cryptography. They detect torn writes,
// accidental corruption and unsophisticated tampering. They provide no
// authentication and no confidentiality; see docs/PERSISTENCE.md for the exact
// trust boundary.
// ---------------------------------------------------------------------------

namespace detail {

constexpr std::uint32_t crc32c_entry(std::uint32_t index) noexcept {
  std::uint32_t crc = index;
  for (int bit = 0; bit < 8; ++bit) {
    crc = ((crc & 1U) != 0U) ? (0x82F63B78U ^ (crc >> 1)) : (crc >> 1);
  }
  return crc;
}

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256U; ++i) {
    table[static_cast<std::size_t>(i)] = crc32c_entry(i);
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();
inline constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

}  // namespace detail

/// CRC-32C (Castagnoli). Returns 0 for empty input with a zero seed.
std::uint32_t crc32c(const std::uint8_t* data, std::size_t size, std::uint32_t seed = 0U) noexcept;
std::uint32_t crc32c(std::string_view data, std::uint32_t seed = 0U) noexcept;

/// FNV-1a 64-bit. Deterministic across platforms and builds.
std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size,
                      std::uint64_t seed = detail::kFnvOffset) noexcept;

/// Incremental FNV-1a 64 digest used to derive canonical object identities.
class Digest64 {
 public:
  Digest64() noexcept = default;

  void reset() noexcept { state_ = detail::kFnvOffset; }
  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept;
  void update_u8(std::uint8_t value) noexcept;
  void update_u16(std::uint16_t value) noexcept;
  void update_u32(std::uint32_t value) noexcept;
  void update_u64(std::uint64_t value) noexcept;
  void update_bool(bool value) noexcept;
  [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_ = detail::kFnvOffset;
};

// ---------------------------------------------------------------------------
// Checked arithmetic. Wraparound is never acceptable in accounting or
// allocation paths, so every arithmetic step that can overflow returns
// std::nullopt instead of a wrapped result.
// ---------------------------------------------------------------------------

template <class T>
constexpr std::optional<T> checked_add(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "checked_add requires an integral type");
  static_assert(!std::is_same_v<T, bool>, "checked_add requires a numeric type");
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
      return std::nullopt;
    }
    return static_cast<T>(a + b);
  } else {
    const T max = std::numeric_limits<T>::max();
    const T min = std::numeric_limits<T>::min();
    if (b > 0 && a > static_cast<T>(max - b)) return std::nullopt;
    if (b < 0 && a < static_cast<T>(min - b)) return std::nullopt;
    return static_cast<T>(a + b);
  }
}

template <class T>
constexpr std::optional<T> checked_sub(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "checked_sub requires an integral type");
  if constexpr (std::is_unsigned_v<T>) {
    if (a < b) return std::nullopt;
    return static_cast<T>(a - b);
  } else {
    const T max = std::numeric_limits<T>::max();
    const T min = std::numeric_limits<T>::min();
    if (b < 0 && a > static_cast<T>(max + b)) return std::nullopt;
    if (b > 0 && a < static_cast<T>(min + b)) return std::nullopt;
    return static_cast<T>(a - b);
  }
}

template <class T>
constexpr std::optional<T> checked_mul(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "checked_mul requires an integral type");
  if (a == 0 || b == 0) return static_cast<T>(0);
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) return std::nullopt;
    return static_cast<T>(a * b);
  } else {
    const T max = std::numeric_limits<T>::max();
    const T min = std::numeric_limits<T>::min();
    if (a > 0) {
      if (b > 0) {
        if (a > max / b) return std::nullopt;
      } else {
        if (b < min / a) return std::nullopt;
      }
    } else {
      if (b > 0) {
        if (a < min / b) return std::nullopt;
      } else {
        if (a != 0 && b < max / a) return std::nullopt;
      }
    }
    return static_cast<T>(a * b);
  }
}

/// Saturating helper for purely advisory counters where overflow must degrade
/// to "at most" rather than abort. Accounting counters never use this.
constexpr std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  const auto sum = checked_add(a, b);
  return sum.has_value() ? *sum : std::numeric_limits<std::uint64_t>::max();
}

// ---------------------------------------------------------------------------
// Error taxonomy.
// ---------------------------------------------------------------------------

enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  OutOfRange = 2,
  CapacityExceeded = 3,
  Overflow = 4,
  NotFound = 5,
  AlreadyExists = 6,
  InvalidState = 7,
  Unsupported = 8,
  Unknown = 9,
  Stale = 10,
  Conflict = 11,
  Invalid = 12,
  Corrupt = 13,
  Truncated = 14,
  VersionUnsupported = 15,
  IntegrityFailure = 16,
  SequenceRegression = 17,
  ReplayDetected = 18,
  Unauthorized = 19,
  InvalidEnum = 20,
  TrailingGarbage = 21,
  SearchLimitReached = 22,
  Indeterminate = 23,
  Refused = 24,
  IoError = 25,
  Shutdown = 26,
  Exhausted = 27,
  NotSupportedByHost = 28,
  Underflow = 29,
  QuotaExceeded = 30,
};

std::string_view to_string(ErrorCode code) noexcept;
/// True for every defined enumerator. Used by wire and durable decoders so an
/// undefined code can never be reinterpreted as a legal one.
bool is_valid(ErrorCode code) noexcept;

/// A code plus a bounded human-readable detail. Detail is truncated, never
/// allocated unboundedly, and is never parsed back by the runtime.
class Status {
 public:
  Status() noexcept = default;
  Status(ErrorCode code) noexcept : code_(code) {}  // NOLINT: implicit by design
  Status(ErrorCode code, std::string_view detail) noexcept;

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  /// Canonical single-line rendering used by tools and tests.
  [[nodiscard]] std::string render() const;

  friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_;
  }
  friend bool operator!=(const Status& a, const Status& b) noexcept { return !(a == b); }

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string detail_{};
};

template <class T>
class Result {
 public:
  Result(T value) noexcept : value_(std::move(value)), ok_(true) {}  // NOLINT: implicit
  Result(Status status) noexcept : status_(std::move(status)), ok_(false) {}  // NOLINT

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] const T& value() const noexcept { return value_; }
  [[nodiscard]] T& value() noexcept { return value_; }
  [[nodiscard]] T value_or(T fallback) const { return ok_ ? value_ : std::move(fallback); }

 private:
  T value_{};
  Status status_{};
  bool ok_ = false;
};

// ---------------------------------------------------------------------------
// Monotonic logical time. Ticks are supplied by the embedding runtime; tests
// drive a manual source so every temporal assertion is deterministic. Ticks are
// never wall-clock time and must never be compared across incarnations.
// ---------------------------------------------------------------------------

struct Tick {
  std::uint64_t value = 0;

  friend bool operator==(Tick a, Tick b) noexcept { return a.value == b.value; }
  friend bool operator!=(Tick a, Tick b) noexcept { return a.value != b.value; }
  friend bool operator<(Tick a, Tick b) noexcept { return a.value < b.value; }
  friend bool operator<=(Tick a, Tick b) noexcept { return a.value <= b.value; }
  friend bool operator>(Tick a, Tick b) noexcept { return a.value > b.value; }
  friend bool operator>=(Tick a, Tick b) noexcept { return a.value >= b.value; }

  [[nodiscard]] std::optional<Tick> next() const noexcept {
    const auto advanced = checked_add(value, std::uint64_t{1});
    if (!advanced.has_value()) return std::nullopt;
    return Tick{*advanced};
  }
};

/// Injectable monotonic source. Implementations must never move backwards.
class TickSource {
 public:
  TickSource() = default;
  virtual ~TickSource() = default;
  TickSource(const TickSource&) = delete;
  TickSource& operator=(const TickSource&) = delete;

  [[nodiscard]] virtual Tick now() const noexcept = 0;
};

/// Deterministic tick source for tests and for replay of recorded sessions.
class ManualTickSource final : public TickSource {
 public:
  explicit ManualTickSource(Tick start = Tick{1}) noexcept : current_(start) {}
  [[nodiscard]] Tick now() const noexcept override { return current_; }
  Tick advance(std::uint64_t delta) noexcept {
    const auto advanced = checked_add(current_.value, delta);
    current_.value = advanced.has_value() ? *advanced : std::numeric_limits<std::uint64_t>::max();
    return current_;
  }
  void set(Tick tick) noexcept { current_ = tick; }

 private:
  Tick current_;
};

// ---------------------------------------------------------------------------
// Bounded collections.
// ---------------------------------------------------------------------------

/// Append-only list with a hard element cap. The cap is enforced before the
/// element is materialised so a hostile count cannot drive allocation.
template <class T>
class BoundedList {
 public:
  BoundedList() = default;
  explicit BoundedList(std::size_t limit) noexcept : limit_(limit) {}

  [[nodiscard]] Status push(const T& value) {
    if (items_.size() >= limit_) {
      return Status(ErrorCode::CapacityExceeded, "bounded list limit reached");
    }
    items_.push_back(value);
    return Status{};
  }

  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }
  void clear() noexcept { items_.clear(); }

  [[nodiscard]] const std::vector<T>& items() const noexcept { return items_; }
  [[nodiscard]] std::vector<T>& items() noexcept { return items_; }
  [[nodiscard]] const T& operator[](std::size_t index) const noexcept { return items_[index]; }

 private:
  std::vector<T> items_{};
  std::size_t limit_ = 0;
};

/// FIFO history with a fixed cap. Oldest entries are evicted on push, which is
/// the retention semantic required for fences, decisions and journals.
template <class T>
class BoundedHistory {
 public:
  BoundedHistory() = default;
  explicit BoundedHistory(std::size_t limit) noexcept : limit_(limit) {}

  [[nodiscard]] bool push(const T& value) {
    if (limit_ == 0) return true;
    bool evicted = false;
    while (items_.size() >= limit_) {
      items_.pop_front();
      evicted = true;
      ++evicted_;
    }
    items_.push_back(value);
    return evicted;
  }

  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }
  [[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_; }
  [[nodiscard]] const std::deque<T>& items() const noexcept { return items_; }
  [[nodiscard]] std::deque<T>& items() noexcept { return items_; }
  void clear() noexcept { items_.clear(); }

 private:
  std::deque<T> items_{};
  std::size_t limit_ = 0;
  std::uint64_t evicted_ = 0;
};

// ---------------------------------------------------------------------------
// Canonical byte encoding.
//
// All multi-byte integers are little-endian and fixed width. Strings are
// length-prefixed with a u32 and validated against kMaxBlobBytes before any
// allocation. The writer marks itself failed on overflow; the reader is total
// and sticky-failing: after the first error every further read is a no-op and
// ok() stays false forever.
// ---------------------------------------------------------------------------

class ByteWriter {
 public:
  explicit ByteWriter(std::size_t max_total = kMaxDocumentBytes,
                      std::size_t max_blob = kMaxBlobBytes) noexcept
      : max_total_(max_total), max_blob_(max_blob) {}

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void boolean(bool value);
  void raw(const void* data, std::size_t size);
  void blob(std::string_view text);
  void padding(std::size_t count);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return data_; }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_.data(); }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view(reinterpret_cast<const char*>(data_.data()), data_.size());
  }

 private:
  /// Checks the bound for \p count more bytes without reserving capacity. The
  /// writer must not reserve exactly the required size: doing so pins capacity
  /// to size on every write and turns a large document into quadratic copying.
  /// Growth is left to the container's geometric strategy.
  [[nodiscard]] bool ensure(std::size_t count) noexcept;

  std::vector<std::uint8_t> data_{};
  std::size_t max_total_;
  std::size_t max_blob_;
  bool ok_ = true;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size,
             std::size_t max_blob = kMaxBlobBytes) noexcept
      : data_(data), size_(size), max_blob_(max_blob) {}
  ByteReader(const std::vector<std::uint8_t>& data,
             std::size_t max_blob = kMaxBlobBytes) noexcept
      : data_(data.data()), size_(data.size()), max_blob_(max_blob) {}
  explicit ByteReader(std::string_view data,
                      std::size_t max_blob = kMaxBlobBytes) noexcept
      : data_(reinterpret_cast<const std::uint8_t*>(data.data())),
        size_(data.size()),
        max_blob_(max_blob) {}

  [[nodiscard]] bool ok() const noexcept { return error_ == ErrorCode::Ok; }
  [[nodiscard]] ErrorCode error() const noexcept { return error_; }
  [[nodiscard]] std::size_t consumed() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - position_; }
  [[nodiscard]] bool at_end() const noexcept { return position_ == size_; }

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  bool boolean();
  std::string_view blob();
  std::string_view raw(std::size_t count);

  /// Reads an element count and validates it against a per-collection bound
  /// *before* the caller allocates. Always returns 0 after a failure.
  std::uint32_t count(std::uint32_t max_allowed) noexcept;

  void poison(ErrorCode code) noexcept;

 private:
  void fail(ErrorCode code) noexcept;

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t position_ = 0;
  std::size_t max_blob_;
  ErrorCode error_ = ErrorCode::Ok;
};

/// Reads an enum encoded as u16 and validates the domain through the is_valid(E)
/// overload found by argument-dependent lookup. On an invalid value the reader is
/// poisoned with InvalidEnum and the fallback is returned, so a malformed
/// enumeration can never be reinterpreted as a legal one.
template <class E>
E read_enum(ByteReader& reader, E fallback) noexcept {
  const std::uint16_t raw = reader.u16();
  if (!reader.ok()) return fallback;
  const E candidate = static_cast<E>(raw);
  if (!is_valid(candidate)) {
    reader.poison(ErrorCode::InvalidEnum);
    return fallback;
  }
  return candidate;
}

/// Writes an enum as its u16 wire representation.
template <class E>
void write_enum(ByteWriter& writer, E value) noexcept {
  writer.u16(static_cast<std::uint16_t>(value));
}

// ---------------------------------------------------------------------------
// Small text helpers.
// ---------------------------------------------------------------------------

std::string to_hex(std::uint64_t value);
std::string to_hex(std::uint32_t value);
/// Renders a bounded, deterministic single-line summary. Never exceeds
/// kMaxDetailBytes.
std::string bounded_detail(std::string_view text, std::size_t limit = kMaxDetailBytes);
/// Returns true when the string contains no control characters other than tab
/// and no NUL. Used before any value is echoed into logs or explanations.
bool is_text_safe(std::string_view text) noexcept;

}  // namespace dmf

#endif  // DMF_CORE_HPP
