// Degraded Mode Fabric - minimal test harness.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deliberately tiny: no third-party dependency, no timeout, no retry. A test
// that hangs is a defect in the runtime, so the harness never hides one.
#ifndef DMF_TESTKIT_TESTKIT_HPP
#define DMF_TESTKIT_TESTKIT_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "dmf/core.hpp"

namespace dmf::test {

/// Thrown by a failed assertion and caught by the runner, which reports it and
/// continues with the next test.
struct AssertionFailure {
  std::string message;
};

void fail(const char* file, int line, const std::string& message);

/// Identity of this process, used to keep scratch directories unique when two
/// suites run concurrently.
std::uint32_t process_id() noexcept;

/// Renders a compared value for a failure message, so a mismatch is diagnosable
/// from the suite output alone.
std::string show(std::uint64_t value);
inline std::string show(std::uint32_t value) { return show(static_cast<std::uint64_t>(value)); }
inline std::string show(std::uint16_t value) { return show(static_cast<std::uint64_t>(value)); }
inline std::string show(unsigned long value) { return show(static_cast<std::uint64_t>(value)); }
inline std::string show(int value) { return show(static_cast<std::uint64_t>(value)); }
inline std::string show(bool value) { return value ? "true" : "false"; }
inline std::string show(const std::string& value) { return value; }
inline std::string show(std::string_view value) { return std::string(value); }
inline std::string show(const char* value) { return std::string(value); }
template <class E, class = std::enable_if_t<std::is_enum_v<E>>>
std::string show(E value) {
  return std::string(::dmf::to_string(value));
}
template <class T, class = std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_enum_v<T>>,
          class = void>
std::string show(const T&) {
  return "<value>";
}

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body);
};

/// Runs every registered test in a deterministic order. Returns 0 on success.
int run_all(int argc, char** argv);

/// Creates a unique directory, removes it on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& label);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string file(const std::string& name) const;

 private:
  std::string path_{};
};

/// Deterministic pseudo-random generator. Every property test seeds it with an
/// explicit value so a failure can be reproduced exactly.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}
  std::uint64_t next() noexcept;
  std::uint64_t below(std::uint64_t bound) noexcept;
  std::uint32_t u32(std::uint32_t bound) noexcept;
  bool coin() noexcept;

 private:
  std::uint64_t state_;
};

}  // namespace dmf::test

#define DMF_TEST(suite_name, test_name)                                                     \
  static void suite_name##_##test_name##_body();                                            \
  static const ::dmf::test::Registrar suite_name##_##test_name##_registrar(                 \
      #suite_name, #test_name, suite_name##_##test_name##_body);                            \
  static void suite_name##_##test_name##_body()

#define DMF_CHECK(condition)                                                              \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      ::dmf::test::fail(__FILE__, __LINE__, "expected: " #condition);                      \
    }                                                                                     \
  } while (false)

#define DMF_CHECK_EQ(actual, expected)                                                    \
  do {                                                                                    \
    const auto& dmf_actual_ = (actual);                                                    \
    const auto& dmf_expected_ = (expected);                                                \
    if (!(dmf_actual_ == dmf_expected_)) {                                                 \
      ::dmf::test::fail(__FILE__, __LINE__,                                                \
                        std::string("expected ") + #actual + " == " + #expected +          \
                            " (actual=" + ::dmf::test::show(dmf_actual_) +                 \
                            ", expected=" + ::dmf::test::show(dmf_expected_) + ")");       \
    }                                                                                     \
  } while (false)

#define DMF_CHECK_OK(expression)                                                          \
  do {                                                                                    \
    const ::dmf::Status dmf_status_ = (expression);                                        \
    if (!dmf_status_.ok()) {                                                               \
      ::dmf::test::fail(__FILE__, __LINE__,                                                \
                        std::string(#expression " failed: ") + dmf_status_.render());      \
    }                                                                                     \
  } while (false)

#define DMF_CHECK_CODE(expression, expected_code)                                         \
  do {                                                                                    \
    const ::dmf::Status dmf_status_ = (expression);                                        \
    if (dmf_status_.code() != (expected_code)) {                                           \
      ::dmf::test::fail(__FILE__, __LINE__,                                                \
                        std::string(#expression " returned ") +                            \
                            std::string(::dmf::to_string(dmf_status_.code())) +            \
                            " instead of " + std::string(::dmf::to_string(expected_code)));\
    }                                                                                     \
  } while (false)

#define DMF_CHECK_THROWS(expression)                                                      \
  do {                                                                                    \
    bool dmf_threw_ = false;                                                               \
    try {                                                                                  \
      (void)(expression);                                                                  \
    } catch (...) {                                                                        \
      dmf_threw_ = true;                                                                   \
    }                                                                                      \
    if (!dmf_threw_) {                                                                     \
      ::dmf::test::fail(__FILE__, __LINE__, #expression " did not throw");                  \
    }                                                                                      \
  } while (false)

#endif  // DMF_TESTKIT_TESTKIT_HPP
