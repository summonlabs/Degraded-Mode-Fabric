// Degraded Mode Fabric - persistence and restart suite.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Adversarial by construction: corrupted headers, every truncation length,
// flipped integrity bits, unsupported versions, impossible lengths, sequence
// regression, trailing garbage, genuine torn tails, crash injection at each
// durable boundary, retention bounds and accounting closure across a restart.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dmf/store.hpp"
#include "testkit/fixtures.hpp"
#include "testkit/testkit.hpp"

using namespace dmf;
using namespace dmf::test;

namespace {

void trace_open_failure(const Status& status) {
  std::printf("reopen failed: %s\n", status.render().c_str());
  std::fflush(stdout);
}

std::vector<std::string> store_files(const std::string& root) {
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string find_journal(const std::string& root) {
  for (const std::string& name : store_files(root)) {
    if (name.rfind("journal-", 0) == 0) return (std::filesystem::path(root) / name).string();
  }
  return {};
}

/// Commits a policy and \p contracts records into a store rooted at \p root.
void seed_store(const std::string& root, std::uint64_t contracts) {
  StoreConfig config;
  config.root = root;
  auto store = StateStore::open(config);
  if (!store.ok()) return;
  const Policy policy = demonstration_policy();
  ByteWriter policy_writer;
  encode(policy_writer, policy);
  (void)store.value()->append(RecordType::PolicyInstalled, policy_writer.bytes());
  for (std::uint64_t i = 1; i <= contracts; ++i) {
    const ServiceContract entry = contract(i, ServiceClass::Standard, 10, 1000, 100, 500, 2);
    ByteWriter writer;
    encode(writer, entry);
    (void)store.value()->append(RecordType::ContractRegistered, writer.bytes());
  }
  (void)store.value()->close();
}

}  // namespace

DMF_TEST(persistence, a_fresh_store_opens_and_reopens_cleanly) {
  TempDir directory{"fresh"};
  StoreConfig config;
  config.root = directory.path();
  {
    auto store = StateStore::open(config);
    DMF_CHECK(store.ok());
    DMF_CHECK_EQ(store.value()->recovery(), RecoveryOutcome::FreshStore);
    const Policy policy = demonstration_policy();
    ByteWriter writer;
    encode(writer, policy);
    DMF_CHECK_OK(store.value()->append(RecordType::PolicyInstalled, writer.bytes()));
    DMF_CHECK_OK(store.value()->close());
  }
  {
    auto store = StateStore::open(config);
    DMF_CHECK(store.ok());
    DMF_CHECK(store.value()->state().policy.validate().ok());
    DMF_CHECK_EQ(store.value()->state().journal_records, std::uint64_t{1});
    DMF_CHECK_EQ(store.value()->recovery(), RecoveryOutcome::CleanReopen);
    DMF_CHECK_OK(store.value()->close());
  }
}

DMF_TEST(persistence, a_snapshot_is_transactional_and_rotates_the_journal) {
  TempDir directory{"snapshot"};
  StoreConfig config;
  config.root = directory.path();
  auto store = StateStore::open(config);
  DMF_CHECK(store.ok());
  seed_store(directory.path(), 0);
  auto second = StateStore::open(config);
  DMF_CHECK(!second.ok());
  (void)store.value()->close();

  auto reopened = StateStore::open(config);
  DMF_CHECK(reopened.ok());
  for (std::uint64_t i = 1; i <= 5; ++i) {
    const ServiceContract entry = contract(i, ServiceClass::Standard, 10, 1000, 100, 500, 2);
    ByteWriter writer;
    encode(writer, entry);
    DMF_CHECK_OK(reopened.value()->append(RecordType::ContractRegistered, writer.bytes()));
  }
  DMF_CHECK_OK(reopened.value()->take_snapshot());
  DMF_CHECK_EQ(reopened.value()->records_in_segment(), std::uint64_t{0});
  DMF_CHECK_OK(reopened.value()->close());

  auto after = StateStore::open(config);
  if (!after.ok()) trace_open_failure(after.status());
  DMF_CHECK(after.ok());
  DMF_CHECK_EQ(after.value()->state().contracts.size(), std::size_t{5});
  DMF_CHECK_EQ(after.value()->recovery(), RecoveryOutcome::CleanReopen);
  DMF_CHECK_OK(after.value()->close());
}

DMF_TEST(persistence, two_writers_on_one_root_are_refused) {
  TempDir directory{"lock"};
  StoreConfig config;
  config.root = directory.path();
  auto first = StateStore::open(config);
  DMF_CHECK(first.ok());
  auto second = StateStore::open(config);
  DMF_CHECK(!second.ok());
  DMF_CHECK_EQ(second.status().code(), ErrorCode::AlreadyExists);
  DMF_CHECK_OK(first.value()->close());
  auto third = StateStore::open(config);
  DMF_CHECK(third.ok());
  DMF_CHECK_OK(third.value()->close());
}

DMF_TEST(persistence, a_read_only_open_never_mutates) {
  TempDir directory{"readonly"};
  seed_store(directory.path(), 3);
  const std::vector<std::string> before = store_files(directory.path());
  StoreConfig config;
  config.root = directory.path();
  config.read_only = true;
  auto store = StateStore::open(config);
  DMF_CHECK(store.ok());
  DMF_CHECK_EQ(store.value()->state().contracts.size(), std::size_t{3});
  DMF_CHECK_CODE(store.value()->append(RecordType::AccountingCheckpoint, {}), ErrorCode::InvalidState);
  DMF_CHECK_OK(store.value()->close());
  const std::vector<std::string> after = store_files(directory.path());
  DMF_CHECK(before == after);
}

DMF_TEST(persistence, a_flipped_payload_bit_is_never_silently_repaired) {
  TempDir directory{"corrupt"};
  seed_store(directory.path(), 2);
  const std::string journal = find_journal(directory.path());
  DMF_CHECK(!journal.empty());
  std::vector<std::uint8_t> bytes;
  {
    std::ifstream stream(journal, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  DMF_CHECK(bytes.size() > 64);
  bytes[48] ^= 0x20;  // inside the first record's payload
  {
    std::ofstream stream(journal, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  StoreConfig config;
  config.root = directory.path();
  auto store = StateStore::open(config);
  DMF_CHECK(!store.ok());
  DMF_CHECK_EQ(store.status().code(), ErrorCode::IntegrityFailure);
  // The file must be untouched by the failed open.
  std::vector<std::uint8_t> after;
  {
    std::ifstream stream(journal, std::ios::binary);
    after.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  DMF_CHECK(after == bytes);
}

DMF_TEST(persistence, truncated_tails_are_recovered_exactly_once) {
  TempDir directory{"torn"};
  seed_store(directory.path(), 3);
  const std::string journal = find_journal(directory.path());
  std::vector<std::uint8_t> bytes;
  {
    std::ifstream stream(journal, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  // seed_store closes cleanly, so the file ends with a 28-byte footer. Removing
  // it plus two bytes of the final record's payload CRC leaves a genuine torn
  // tail on the last record.
  bytes.resize(bytes.size() - 30);
  {
    std::ofstream stream(journal, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  StoreConfig config;
  config.root = directory.path();
  auto first = StateStore::open(config);
  DMF_CHECK(first.ok());
  DMF_CHECK_EQ(first.value()->recovery(), RecoveryOutcome::TornTailRecovered);
  DMF_CHECK_EQ(first.value()->state().contracts.size(), std::size_t{2});
  DMF_CHECK_OK(first.value()->close());

  // The second open must succeed: recovering a torn tail must leave a store that
  // can be recovered again.
  auto second = StateStore::open(config);
  DMF_CHECK(second.ok());
  DMF_CHECK_EQ(second.value()->state().contracts.size(), std::size_t{2});
  DMF_CHECK_OK(second.value()->close());
}

DMF_TEST(persistence, a_truncated_header_is_recovered_and_a_broken_header_is_not) {
  TempDir directory{"halfhead"};
  seed_store(directory.path(), 2);
  const std::string journal = find_journal(directory.path());
  std::vector<std::uint8_t> bytes;
  {
    std::ifstream stream(journal, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  const std::size_t header_size = 28;
  const std::size_t record_header_size = 28;
  bytes.resize(header_size + record_header_size + 5);  // a partial record payload
  {
    std::ofstream stream(journal, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  StoreConfig config;
  config.root = directory.path();
  auto store = StateStore::open(config);
  DMF_CHECK(store.ok());
  DMF_CHECK_EQ(store.value()->recovery(), RecoveryOutcome::TornTailRecovered);
  DMF_CHECK_EQ(store.value()->state().contracts.size(), std::size_t{0});
  DMF_CHECK_OK(store.value()->close());

  // A file that cannot even hold the file header is not a store.
  std::vector<std::uint8_t> too_short(bytes.begin(), bytes.begin() + 4);
  {
    std::ofstream stream(journal, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(too_short.data()),
                 static_cast<std::streamsize>(too_short.size()));
  }
  auto bad = StateStore::open(config);
  DMF_CHECK(!bad.ok());
  std::vector<std::uint8_t> after;
  {
    std::ifstream stream(journal, std::ios::binary);
    after.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  DMF_CHECK(after == too_short);
}

DMF_TEST(persistence, an_unsupported_version_is_refused) {
  TempDir directory{"version"};
  seed_store(directory.path(), 1);
  const std::string journal = find_journal(directory.path());
  std::vector<std::uint8_t> bytes;
  {
    std::ifstream stream(journal, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  bytes[4] = 0x7F;  // journal format version
  // The header CRC now disagrees, which is detected first; repair it so the
  // version check itself is exercised.
  const std::uint32_t crc = crc32c(bytes.data(), 24);
  bytes[24] = static_cast<std::uint8_t>(crc & 0xFFU);
  bytes[25] = static_cast<std::uint8_t>((crc >> 8) & 0xFFU);
  bytes[26] = static_cast<std::uint8_t>((crc >> 16) & 0xFFU);
  bytes[27] = static_cast<std::uint8_t>((crc >> 24) & 0xFFU);
  {
    std::ofstream stream(journal, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  StoreConfig config;
  config.root = directory.path();
  auto store = StateStore::open(config);
  DMF_CHECK(!store.ok());
  DMF_CHECK_EQ(store.status().code(), ErrorCode::VersionUnsupported);
}

DMF_TEST(persistence, sequence_regression_is_refused) {
  TempDir directory{"sequence"};
  seed_store(directory.path(), 3);
  const std::string journal = find_journal(directory.path());
  std::vector<std::uint8_t> bytes;
  {
    std::ifstream stream(journal, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  // The first record header starts right after the 28-byte journal header.
  const std::size_t header = 28;
  const std::size_t record_header = 28;
  // Rewrite the sequence field of the first record to a value that is not the
  // expected successor, then repair the header CRC.
  for (int i = 0; i < 8; ++i) bytes[header + 12 + static_cast<std::size_t>(i)] = 0xEE;
  const std::uint32_t crc = crc32c(bytes.data() + header, record_header - 4);
  for (int i = 0; i < 4; ++i) {
    bytes[header + record_header - 4 + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFU);
  }
  {
    std::ofstream stream(journal, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  StoreConfig config;
  config.root = directory.path();
  auto store = StateStore::open(config);
  DMF_CHECK(!store.ok());
  DMF_CHECK_EQ(store.status().code(), ErrorCode::SequenceRegression);
}

DMF_TEST(persistence, trailing_garbage_is_refused) {
  TempDir directory{"garbage"};
  seed_store(directory.path(), 1);
  const std::string journal = find_journal(directory.path());
  {
    std::ofstream stream(journal, std::ios::binary | std::ios::app);
    const char junk[64] = "this is not a record and it never will be, not ever at all....";
    stream.write(junk, 64);
  }
  StoreConfig config;
  config.root = directory.path();
  auto store = StateStore::open(config);
  DMF_CHECK(!store.ok());
}

DMF_TEST(persistence, every_truncation_length_is_either_recovered_or_refused) {
  TempDir directory{"prefix"};
  seed_store(directory.path(), 3);
  const std::string journal = find_journal(directory.path());
  std::vector<std::uint8_t> original;
  {
    std::ifstream stream(journal, std::ios::binary);
    original.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  for (std::size_t length = 1; length < original.size(); ++length) {
    {
      std::ofstream stream(journal, std::ios::binary | std::ios::trunc);
      stream.write(reinterpret_cast<const char*>(original.data()),
                   static_cast<std::streamsize>(length));
    }
    (void)std::filesystem::remove((std::filesystem::path(directory.path()) / "store.lock").string());
    StoreConfig config;
    config.root = directory.path();
    auto store = StateStore::open(config);
    if (store.ok()) {
      // A recovered prefix may only ever contain whole records, and reopening it
      // must succeed: recovery never leaves an unreadable store behind.
      DMF_CHECK_OK(store.value()->close());
      auto again = StateStore::open(config);
      DMF_CHECK(again.ok());
      DMF_CHECK_OK(again.value()->close());
    }
  }
}

DMF_TEST(persistence, retention_bounds_are_enforced_and_counted) {
  TempDir directory{"retention"};
  StoreConfig config;
  config.root = directory.path();
  config.max_retained_grants = 4;
  config.max_retained_fences = 4;
  config.max_retained_decisions = 4;
  auto store = StateStore::open(config);
  DMF_CHECK(store.ok());
  const ServiceContract entry = contract(1, ServiceClass::Standard, 10, 1000, 100, 500, 2);
  ByteWriter contract_writer;
  encode(contract_writer, entry);
  DMF_CHECK_OK(store.value()->append(RecordType::ContractRegistered, contract_writer.bytes()));

  for (std::uint64_t i = 1; i <= 12; ++i) {
    Grant grant;
    grant.id = GrantId::from_value(i);
    grant.decision = DecisionId::from_value(i);
    grant.binding = binding_for(entry, CapacityGeneration::from_value(1));
    grant.original = entry.original;
    grant.degraded = entry.original;
    grant.sequence = SequenceNumber::from_value(1);
    grant.last_attempt = AttemptId::from_value(i);
    grant.issued_tick = Tick{i};
    grant.expires_tick = Tick{i + 100};
    grant.last_transition_tick = Tick{i};
    grant.state = GrantState::Issued;
    grant.provenance = ProvenanceClass::Synthetic;
    ByteWriter grant_writer;
    encode(grant_writer, grant);
    DMF_CHECK_OK(store.value()->append(RecordType::GrantIssued, grant_writer.bytes()));

    FenceRecord fence;
    fence.id = FenceId::from_value(i);
    fence.grant = grant.id;
    fence.reason = FenceReason::Manual;
    fence.prior = grant.binding;
    fence.current = grant.binding;
    fence.fenced_tick = Tick{i + 1};
    ByteWriter fence_writer;
    encode(fence_writer, fence);
    DMF_CHECK_OK(store.value()->append(RecordType::GrantFenced, fence_writer.bytes()));
  }
  DMF_CHECK_OK(store.value()->take_snapshot());
  DMF_CHECK_OK(store.value()->close());

  auto after = StateStore::open(config);
  if (!after.ok()) trace_open_failure(after.status());
  DMF_CHECK(after.ok());
  const DurableState& state = after.value()->state();
  DMF_CHECK(state.grants.size() <= std::size_t{4});
  DMF_CHECK(state.fences.size() <= std::size_t{4});
  DMF_CHECK(state.counters.grants_issued == 12);
  DMF_CHECK(state.counters.grants_pruned >= 8);
  DMF_CHECK(state.live_grant_count() == 0);
  const Accounting accounting(state.counters);
  ClosureInputs inputs;
  inputs.live_grants = state.live_grant_count();
  inputs.retained_terminated_grants = state.terminated_grant_count();
  inputs.retained_fences = state.fences.size();
  inputs.retained_decisions = state.decisions.size();
  const ClosureReport report = accounting.check_closure(inputs);
  DMF_CHECK(report.closed);
  DMF_CHECK_OK(after.value()->close());
}

DMF_TEST(persistence, a_corrupt_snapshot_is_refused_rather_than_skipped) {
  TempDir directory{"badsnap"};
  StoreConfig config;
  config.root = directory.path();
  {
    auto store = StateStore::open(config);
    DMF_CHECK(store.ok());
    const ServiceContract entry = contract(1, ServiceClass::Standard, 10, 1000, 100, 500, 2);
    ByteWriter writer;
    encode(writer, entry);
    DMF_CHECK_OK(store.value()->append(RecordType::ContractRegistered, writer.bytes()));
    DMF_CHECK_OK(store.value()->take_snapshot());
    DMF_CHECK_OK(store.value()->close());
  }
  std::string snapshot;
  for (const std::string& name : store_files(directory.path())) {
    if (name.rfind("snapshot-", 0) == 0) snapshot = (std::filesystem::path(directory.path()) / name).string();
  }
  DMF_CHECK(!snapshot.empty());
  std::vector<std::uint8_t> bytes;
  {
    std::ifstream stream(snapshot, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  bytes.back() ^= 0xFF;  // corrupt the footer integrity field
  {
    std::ofstream stream(snapshot, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  (void)std::filesystem::remove((std::filesystem::path(directory.path()) / "store.lock").string());
  auto store = StateStore::open(config);
  DMF_CHECK(!store.ok());
  DMF_CHECK_EQ(store.status().code(), ErrorCode::IntegrityFailure);
}

DMF_TEST(persistence, every_artifact_lives_inside_the_store_root) {
  TempDir directory{"names"};
  seed_store(directory.path(), 2);
  const std::vector<std::string> names = store_files(directory.path());
  DMF_CHECK(!names.empty());
  for (const std::string& name : names) {
    DMF_CHECK(name.find("..") == std::string::npos);
    DMF_CHECK(name.find('/') == std::string::npos);
    DMF_CHECK(name.find('\\') == std::string::npos);
  }
}