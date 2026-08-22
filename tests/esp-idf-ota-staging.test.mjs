import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo, "firmware/inkloop-idf/components/inkloop_ota",
);

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "inkloop/esp_ota_staging.hpp"

using namespace inkloop;

static OtaSha256Digest digestOf(const std::uint8_t* bytes,
                                std::size_t length) {
  OtaSha256 hash;
  assert(hash.update(bytes, length));
  OtaSha256Digest output{};
  assert(hash.finish(output));
  return output;
}

static OtaSha256Digest signedFixtureValue(
    const CanonicalOtaManifest& canonical,
    const OtaSha256Digest& digest) {
  OtaSha256 hash;
  assert(hash.update(canonical.bytes.data(), canonical.length));
  assert(hash.update(digest.data(), digest.size()));
  OtaSha256Digest output{};
  assert(hash.finish(output));
  return output;
}

struct Fixture {
  std::string board = "papercolor-esp32s3";
  std::string version = "2.7.0-rc.1+signed";
  std::string policy = kPinnedEd25519Sha256Policy;
  std::vector<std::uint8_t> image;
  OtaSha256Digest digest{};
  OtaSha256Digest signature{};

  explicit Fixture(std::size_t image_size = 173U) {
    image.resize(image_size);
    for (std::size_t at = 0U; at < image.size(); ++at)
      image[at] = static_cast<std::uint8_t>((at * 37U + 11U) & 0xFFU);
    digest = digestOf(image.data(), image.size());
    signature.fill(0xA5U);
    PreparedOtaManifest prepared;
    assert(prepareOtaManifest(view(), text(board), prepared) ==
           OtaManifestCode::Ok);
    CanonicalOtaManifest canonical;
    assert(canonicalizeOtaManifest(prepared, canonical) ==
           OtaManifestCode::Ok);
    signature = signedFixtureValue(canonical, digest);
  }

  static OtaTextView text(const std::string& value) {
    return {value.data(), value.size()};
  }

  ReviewedOtaManifest view() const {
    ReviewedOtaManifest output;
    output.board_sku = text(board);
    output.firmware_version = text(version);
    output.image_size = image.size();
    output.image_sha256 = digest;
    output.signature_policy = text(policy);
    output.detached_signature = {signature.data(), signature.size()};
    return output;
  }
};

class DeterministicTestVerifier final
    : public IPinnedOtaSignatureVerifier {
 public:
  bool supportsPolicy(OtaTextView policy) const override {
    ++support_calls;
    events += 'P';
    const std::string expected = kPinnedEd25519Sha256Policy;
    return supports && policy.data && policy.length == expected.size() &&
        std::memcmp(policy.data, expected.data(), policy.length) == 0;
  }

  bool verify(OtaTextView, const CanonicalOtaManifest& canonical,
              const OtaSha256Digest& digest,
              OtaBytesView signature) const override {
    ++verify_calls;
    events += 'S';
    const OtaSha256Digest expected = signedFixtureValue(canonical, digest);
    return accepts && signature.data &&
        signature.length == expected.size() &&
        std::memcmp(signature.data, expected.data(), expected.size()) == 0;
  }

  bool supports = true;
  bool accepts = true;
  mutable int support_calls = 0;
  mutable int verify_calls = 0;
  mutable std::string events;
};

struct FakeEsp {
  EspOtaPartition running = nullptr;
  EspOtaPartition target = nullptr;
  std::uint64_t capacity = kMaximumOtaImageBytes;
  int begin_status = 0;
  int end_status = 0;
  int abort_status = 0;
  int select_status = 0;
  int fail_write_call = 0;
  int running_calls = 0;
  int target_calls = 0;
  int capacity_calls = 0;
  int begin_calls = 0;
  int write_calls = 0;
  int end_calls = 0;
  int abort_calls = 0;
  int select_calls = 0;
  std::size_t begin_size = 0U;
  EspOtaPartition begin_partition = nullptr;
  EspOtaPartition selected_partition = nullptr;
  std::vector<std::size_t> write_sizes;
  std::string events;
};

static FakeEsp g_esp;
static int g_running_tag = 1;
static int g_target_tag = 2;

static void resetEsp() {
  g_esp = FakeEsp{};
  g_esp.running = &g_running_tag;
  g_esp.target = &g_target_tag;
}

static EspOtaPartition getRunning() {
  ++g_esp.running_calls;
  g_esp.events += 'R';
  return g_esp.running;
}

static EspOtaPartition getTarget() {
  ++g_esp.target_calls;
  g_esp.events += 'N';
  return g_esp.target;
}

static std::uint64_t capacity(EspOtaPartition partition) {
  ++g_esp.capacity_calls;
  g_esp.events += 'C';
  assert(partition == g_esp.target);
  return g_esp.capacity;
}

static int beginWrite(EspOtaPartition partition, std::size_t size,
                      EspOtaHandle& handle) {
  ++g_esp.begin_calls;
  g_esp.events += 'B';
  g_esp.begin_partition = partition;
  g_esp.begin_size = size;
  if (g_esp.begin_status == 0) handle = 0xC0DEU;
  return g_esp.begin_status;
}

static int writeChunk(EspOtaHandle handle, const std::uint8_t*,
                      std::size_t length) {
  ++g_esp.write_calls;
  g_esp.events += 'W';
  g_esp.write_sizes.push_back(length);
  assert(handle == 0xC0DEU);
  return g_esp.fail_write_call == g_esp.write_calls ? 71 : 0;
}

static int endWrite(EspOtaHandle handle) {
  ++g_esp.end_calls;
  g_esp.events += 'E';
  assert(handle == 0xC0DEU);
  return g_esp.end_status;
}

static int abortWrite(EspOtaHandle handle) {
  ++g_esp.abort_calls;
  g_esp.events += 'A';
  assert(handle == 0xC0DEU);
  return g_esp.abort_status;
}

static int selectTarget(EspOtaPartition partition) {
  ++g_esp.select_calls;
  g_esp.events += 'T';
  g_esp.selected_partition = partition;
  return g_esp.select_status;
}

static const EspOtaWriterFunctions kFunctions{
    &getRunning, &getTarget, &capacity, &beginWrite, &writeChunk,
    &endWrite, &abortWrite, &selectTarget};

static EspOtaStagingObservation beginFixture(
    EspOtaStagingAdapter& adapter, const Fixture& fixture) {
  return adapter.begin(fixture.view(), Fixture::text(fixture.board));
}

static void assertNoMutation() {
  assert(g_esp.begin_calls == 0);
  assert(g_esp.write_calls == 0);
  assert(g_esp.end_calls == 0);
  assert(g_esp.abort_calls == 0);
  assert(g_esp.select_calls == 0);
}

static void shaAndCanonicalMatrix() {
  const std::uint8_t abc[] = {'a', 'b', 'c'};
  const OtaSha256Digest digest = digestOf(abc, sizeof(abc));
  constexpr std::uint8_t expected[32] = {
      0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
      0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
      0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
      0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU};
  assert(std::memcmp(digest.data(), expected, sizeof(expected)) == 0);
  OtaSha256 used;
  OtaSha256Digest output{};
  assert(used.update(abc, sizeof(abc)));
  assert(used.finish(output));
  assert(!used.update(abc, sizeof(abc)));
  assert(!used.finish(output));

  Fixture fixture;
  PreparedOtaManifest prepared;
  assert(prepareOtaManifest(fixture.view(), Fixture::text(fixture.board),
                            prepared) == OtaManifestCode::Ok);
  CanonicalOtaManifest first;
  CanonicalOtaManifest second;
  assert(canonicalizeOtaManifest(prepared, first) == OtaManifestCode::Ok);
  assert(canonicalizeOtaManifest(prepared, second) == OtaManifestCode::Ok);
  assert(first.length == second.length);
  assert(std::memcmp(first.bytes.data(), second.bytes.data(), first.length) ==
         0);
  const std::string canonical_text(
      reinterpret_cast<const char*>(first.bytes.data()), first.length);
  assert(canonical_text.find("INKLOOP-OTA-MANIFEST-V1") !=
         std::string::npos);
  assert(canonical_text.find(fixture.board) != std::string::npos);
  assert(canonical_text.find(fixture.version) != std::string::npos);
  assert(canonical_text.find(fixture.policy) != std::string::npos);

  PreparedOtaManifest changed = prepared;
  changed.detached_signature[0] ^= 0xFFU;
  CanonicalOtaManifest signature_changed;
  assert(canonicalizeOtaManifest(changed, signature_changed) ==
         OtaManifestCode::Ok);
  assert(signature_changed.length == first.length);
  assert(std::memcmp(signature_changed.bytes.data(), first.bytes.data(),
                     first.length) == 0);

  changed = prepared;
  changed.board_sku[0] = changed.board_sku[0] == 'p' ? 'q' : 'p';
  CanonicalOtaManifest board_changed;
  assert(canonicalizeOtaManifest(changed, board_changed) ==
         OtaManifestCode::Ok);
  assert(std::memcmp(board_changed.bytes.data(), first.bytes.data(),
                     first.length) != 0);
  changed = prepared;
  changed.firmware_version[0] = '9';
  CanonicalOtaManifest version_changed;
  assert(canonicalizeOtaManifest(changed, version_changed) ==
         OtaManifestCode::Ok);
  assert(std::memcmp(version_changed.bytes.data(), first.bytes.data(),
                     first.length) != 0);
  changed = prepared;
  ++changed.image_size;
  CanonicalOtaManifest size_changed;
  assert(canonicalizeOtaManifest(changed, size_changed) ==
         OtaManifestCode::Ok);
  assert(std::memcmp(size_changed.bytes.data(), first.bytes.data(),
                     first.length) != 0);
  changed = prepared;
  changed.image_sha256[0] ^= 1U;
  CanonicalOtaManifest digest_changed;
  assert(canonicalizeOtaManifest(changed, digest_changed) ==
         OtaManifestCode::Ok);
  assert(std::memcmp(digest_changed.bytes.data(), first.bytes.data(),
                     first.length) != 0);
}

static void rejectedManifest(const ReviewedOtaManifest& manifest,
                             OtaTextView device) {
  resetEsp();
  DeterministicTestVerifier verifier;
  EspOtaStagingAdapter adapter(kFunctions, &verifier);
  const EspOtaStagingObservation result = adapter.begin(manifest, device);
  assert(result.code == EspOtaStagingCode::ManifestRejected);
  assert(adapter.core().state() == OtaStagingState::Failed);
  assert(g_esp.running_calls == 0 && g_esp.target_calls == 0);
  assertNoMutation();
}

static void manifestRejectionMatrix() {
  Fixture fixture;
  const OtaTextView device = Fixture::text(fixture.board);
  ReviewedOtaManifest input = fixture.view();
  input.schema_version = 2U;
  rejectedManifest(input, device);
  input = fixture.view();
  input.board_sku = {nullptr, 1U};
  rejectedManifest(input, device);
  input = fixture.view();
  input.board_sku.length = 0U;
  rejectedManifest(input, device);
  std::string large_board(kMaximumOtaBoardSkuBytes + 1U, 'b');
  input = fixture.view();
  input.board_sku = Fixture::text(large_board);
  rejectedManifest(input, device);
  std::string bad_board = "paper/color";
  input = fixture.view();
  input.board_sku = Fixture::text(bad_board);
  rejectedManifest(input, Fixture::text(bad_board));
  std::string other_board = "other-esp32s3";
  input = fixture.view();
  input.board_sku = Fixture::text(other_board);
  rejectedManifest(input, device);
  rejectedManifest(fixture.view(), {nullptr, fixture.board.size()});

  input = fixture.view();
  input.firmware_version = {nullptr, 1U};
  rejectedManifest(input, device);
  input = fixture.view();
  input.firmware_version.length = 0U;
  rejectedManifest(input, device);
  std::string large_version(kMaximumOtaFirmwareVersionBytes + 1U, 'v');
  input = fixture.view();
  input.firmware_version = Fixture::text(large_version);
  rejectedManifest(input, device);
  std::string bad_version = "v2/escape";
  input = fixture.view();
  input.firmware_version = Fixture::text(bad_version);
  rejectedManifest(input, device);

  input = fixture.view();
  input.image_size = 0U;
  rejectedManifest(input, device);
  input = fixture.view();
  input.image_size = kMaximumOtaImageBytes + 1U;
  rejectedManifest(input, device);
  input = fixture.view();
  input.image_sha256.fill(0U);
  rejectedManifest(input, device);

  std::string unknown = "unknown-signing-v1";
  input = fixture.view();
  input.signature_policy = Fixture::text(unknown);
  rejectedManifest(input, device);
  input = fixture.view();
  input.signature_policy = {nullptr, 1U};
  rejectedManifest(input, device);
  std::string large_policy(kMaximumOtaSignaturePolicyBytes + 1U, 'p');
  input = fixture.view();
  input.signature_policy = Fixture::text(large_policy);
  rejectedManifest(input, device);

  input = fixture.view();
  input.detached_signature = {nullptr, 1U};
  rejectedManifest(input, device);
  input = fixture.view();
  input.detached_signature.length = 0U;
  rejectedManifest(input, device);
  std::vector<std::uint8_t> huge_signature(
      kMaximumOtaDetachedSignatureBytes + 1U, 0x5AU);
  input = fixture.view();
  input.detached_signature = {huge_signature.data(), huge_signature.size()};
  rejectedManifest(input, device);

  resetEsp();
  EspOtaStagingAdapter no_verifier(kFunctions, nullptr);
  assert(beginFixture(no_verifier, fixture).code ==
         EspOtaStagingCode::VerifierUnavailable);
  assert(g_esp.running_calls == 0 && g_esp.target_calls == 0);
  assertNoMutation();

  resetEsp();
  DeterministicTestVerifier unsupported;
  unsupported.supports = false;
  EspOtaStagingAdapter unsupported_adapter(kFunctions, &unsupported);
  assert(beginFixture(unsupported_adapter, fixture).code ==
         EspOtaStagingCode::SignaturePolicyUnsupported);
  assert(g_esp.running_calls == 0 && g_esp.target_calls == 0);
  assertNoMutation();
}

static void invalidFunctionsMatrix() {
  Fixture fixture;
  DeterministicTestVerifier verifier;
  std::array<EspOtaWriterFunctions, 8> invalid{};
  invalid.fill(kFunctions);
  invalid[0].get_running_partition = nullptr;
  invalid[1].get_next_update_partition = nullptr;
  invalid[2].partition_capacity = nullptr;
  invalid[3].ota_begin = nullptr;
  invalid[4].ota_write = nullptr;
  invalid[5].ota_end = nullptr;
  invalid[6].ota_abort = nullptr;
  invalid[7].set_boot_partition = nullptr;
  for (const EspOtaWriterFunctions& functions : invalid) {
    resetEsp();
    EspOtaStagingAdapter adapter(functions, &verifier);
    assert(beginFixture(adapter, fixture).code ==
           EspOtaStagingCode::InvalidFunctions);
    assert(g_esp.running_calls == 0 && g_esp.target_calls == 0);
    assertNoMutation();
  }
}

static void partitionBoundaryMatrix() {
  Fixture fixture;
  DeterministicTestVerifier verifier;
  resetEsp();
  g_esp.running = nullptr;
  EspOtaStagingAdapter no_running(kFunctions, &verifier);
  assert(beginFixture(no_running, fixture).code ==
         EspOtaStagingCode::RunningPartitionUnavailable);
  assert(g_esp.target_calls == 0);
  assertNoMutation();

  resetEsp();
  g_esp.target = nullptr;
  EspOtaStagingAdapter no_target(kFunctions, &verifier);
  assert(beginFixture(no_target, fixture).code ==
         EspOtaStagingCode::TargetPartitionUnavailable);
  assertNoMutation();

  resetEsp();
  g_esp.target = g_esp.running;
  EspOtaStagingAdapter alias(kFunctions, &verifier);
  assert(beginFixture(alias, fixture).code ==
         EspOtaStagingCode::TargetAliasesRunning);
  assertNoMutation();

  resetEsp();
  g_esp.capacity = fixture.image.size() - 1U;
  EspOtaStagingAdapter too_small(kFunctions, &verifier);
  assert(beginFixture(too_small, fixture).code ==
         EspOtaStagingCode::TargetTooSmall);
  assertNoMutation();

  resetEsp();
  g_esp.begin_status = 19;
  EspOtaStagingAdapter begin_failure(kFunctions, &verifier);
  const EspOtaStagingObservation begin_result =
      beginFixture(begin_failure, fixture);
  assert(begin_result.code == EspOtaStagingCode::BeginFailed);
  assert(begin_result.system_status == 19);
  assert(g_esp.begin_calls == 1 && g_esp.abort_calls == 0);
  assert(g_esp.select_calls == 0);
}

static const std::array<std::size_t, 3> kChunks{{17U, 64U, 92U}};

static void writeAll(EspOtaStagingAdapter& adapter,
                     const Fixture& fixture) {
  std::size_t offset = 0U;
  for (const std::size_t length : kChunks) {
    assert(adapter.write(fixture.image.data() + offset, length).code ==
           EspOtaStagingCode::Ok);
    offset += length;
  }
  assert(offset == fixture.image.size());
}

static void writeAndFinalizeFaultMatrix() {
  Fixture fixture;
  assert(fixture.image.size() == 173U);
  for (int fault_call = 1; fault_call <= 3; ++fault_call) {
    resetEsp();
    g_esp.fail_write_call = fault_call;
    DeterministicTestVerifier verifier;
    EspOtaStagingAdapter adapter(kFunctions, &verifier);
    assert(beginFixture(adapter, fixture).code == EspOtaStagingCode::Ok);
    std::size_t offset = 0U;
    EspOtaStagingObservation result;
    for (const std::size_t length : kChunks) {
      result = adapter.write(fixture.image.data() + offset, length);
      offset += length;
      if (result.code != EspOtaStagingCode::Ok) break;
    }
    assert(result.code == EspOtaStagingCode::WriteFailed);
    assert(result.system_status == 71);
    assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);
    (void)adapter.abort();
    assert(adapter.write(fixture.image.data(), 1U).code ==
           EspOtaStagingCode::InvalidState);
    assert(adapter.finish().code == EspOtaStagingCode::InvalidState);
    assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);
  }

  for (int invalid = 0; invalid < 3; ++invalid) {
    resetEsp();
    DeterministicTestVerifier verifier;
    EspOtaStagingAdapter adapter(kFunctions, &verifier);
    assert(beginFixture(adapter, fixture).code == EspOtaStagingCode::Ok);
    EspOtaStagingObservation result;
    std::vector<std::uint8_t> oversized(kMaximumOtaChunkBytes + 1U, 0U);
    if (invalid == 0) result = adapter.write(nullptr, 1U);
    if (invalid == 1) result = adapter.write(fixture.image.data(), 0U);
    if (invalid == 2) result = adapter.write(oversized.data(),
                                             oversized.size());
    assert(result.code == EspOtaStagingCode::ChunkRejected);
    assert(g_esp.write_calls == 0 && g_esp.abort_calls == 1);
    assert(g_esp.select_calls == 0);
  }

  resetEsp();
  DeterministicTestVerifier under_verifier;
  EspOtaStagingAdapter under(kFunctions, &under_verifier);
  assert(beginFixture(under, fixture).code == EspOtaStagingCode::Ok);
  assert(under.write(fixture.image.data(), fixture.image.size() - 1U).code ==
         EspOtaStagingCode::Ok);
  EspOtaStagingObservation result = under.finish();
  assert(result.code == EspOtaStagingCode::FinalizeRejected);
  assert(result.core_code == OtaStagingCode::ImageIncomplete);
  assert(g_esp.abort_calls == 1 && g_esp.end_calls == 0 &&
         g_esp.select_calls == 0);

  resetEsp();
  DeterministicTestVerifier overflow_verifier;
  EspOtaStagingAdapter overflow(kFunctions, &overflow_verifier);
  assert(beginFixture(overflow, fixture).code == EspOtaStagingCode::Ok);
  writeAll(overflow, fixture);
  result = overflow.write(fixture.image.data(), 1U);
  assert(result.code == EspOtaStagingCode::ChunkRejected);
  assert(result.core_code == OtaStagingCode::ImageTooLarge);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);

  resetEsp();
  Fixture wrong_digest;
  wrong_digest.digest[0] ^= 0x80U;
  DeterministicTestVerifier digest_verifier;
  EspOtaStagingAdapter digest_adapter(kFunctions, &digest_verifier);
  assert(beginFixture(digest_adapter, wrong_digest).code ==
         EspOtaStagingCode::Ok);
  writeAll(digest_adapter, wrong_digest);
  result = digest_adapter.finish();
  assert(result.code == EspOtaStagingCode::FinalizeRejected);
  assert(result.core_code == OtaStagingCode::DigestMismatch);
  assert(digest_verifier.verify_calls == 0);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);

  resetEsp();
  DeterministicTestVerifier signature_verifier;
  signature_verifier.accepts = false;
  EspOtaStagingAdapter signature_adapter(kFunctions, &signature_verifier);
  assert(beginFixture(signature_adapter, fixture).code ==
         EspOtaStagingCode::Ok);
  writeAll(signature_adapter, fixture);
  result = signature_adapter.finish();
  assert(result.code == EspOtaStagingCode::SignatureRejected);
  assert(result.core_code == OtaStagingCode::SignatureRejected);
  assert(signature_verifier.verify_calls == 1);
  assert(g_esp.end_calls == 0 && g_esp.abort_calls == 1 &&
         g_esp.select_calls == 0);

  resetEsp();
  g_esp.end_status = 23;
  DeterministicTestVerifier end_verifier;
  EspOtaStagingAdapter end_adapter(kFunctions, &end_verifier);
  assert(beginFixture(end_adapter, fixture).code == EspOtaStagingCode::Ok);
  writeAll(end_adapter, fixture);
  result = end_adapter.finish();
  assert(result.code == EspOtaStagingCode::EndFailed);
  assert(result.system_status == 23);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);

  resetEsp();
  g_esp.select_status = 29;
  DeterministicTestVerifier select_verifier;
  EspOtaStagingAdapter select_adapter(kFunctions, &select_verifier);
  assert(beginFixture(select_adapter, fixture).code ==
         EspOtaStagingCode::Ok);
  writeAll(select_adapter, fixture);
  result = select_adapter.finish();
  assert(result.code == EspOtaStagingCode::SelectFailed);
  assert(result.system_status == 29);
  assert(g_esp.end_calls == 1 && g_esp.abort_calls == 1 &&
         g_esp.select_calls == 1);
  assert(!select_adapter.targetSelected());
}

static void successAndAbortMatrix() {
  Fixture fixture;
  resetEsp();
  DeterministicTestVerifier verifier;
  EspOtaStagingAdapter adapter(kFunctions, &verifier);
  assert(beginFixture(adapter, fixture).code == EspOtaStagingCode::Ok);
  assert(g_esp.begin_partition == &g_target_tag);
  assert(g_esp.begin_size == fixture.image.size());
  writeAll(adapter, fixture);
  assert(adapter.finish().code == EspOtaStagingCode::Ok);
  assert(adapter.core().state() == OtaStagingState::BootSelected);
  assert(adapter.targetSelected());
  assert(g_esp.write_sizes ==
         std::vector<std::size_t>(kChunks.begin(), kChunks.end()));
  assert(g_esp.end_calls == 1 && g_esp.select_calls == 1 &&
         g_esp.abort_calls == 0);
  assert(g_esp.selected_partition == &g_target_tag);
  assert(verifier.verify_calls == 1 && verifier.support_calls == 2);
  assert(g_esp.events == "RNCBWWWET");
  assert(verifier.events == "PPS");
  assert(adapter.finish().code == EspOtaStagingCode::InvalidState);
  assert(adapter.abort().code == EspOtaStagingCode::InvalidState);
  assert(g_esp.select_calls == 1 && g_esp.abort_calls == 0);

  resetEsp();
  DeterministicTestVerifier abort_verifier;
  EspOtaStagingAdapter aborted(kFunctions, &abort_verifier);
  assert(beginFixture(aborted, fixture).code == EspOtaStagingCode::Ok);
  assert(aborted.abort().code == EspOtaStagingCode::Aborted);
  assert(aborted.abort().code == EspOtaStagingCode::Aborted);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);
  assert(aborted.write(fixture.image.data(), 1U).code ==
         EspOtaStagingCode::InvalidState);
  assert(aborted.finish().code == EspOtaStagingCode::InvalidState);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);
}

int main() {
  shaAndCanonicalMatrix();
  manifestRejectionMatrix();
  invalidFunctionsMatrix();
  partitionBoundaryMatrix();
  writeAndFinalizeFaultMatrix();
  successAndAbortMatrix();
  assert(std::string(otaManifestCodeName(
             OtaManifestCode::UnknownSignaturePolicy)) ==
         "UNKNOWN_SIGNATURE_POLICY");
  assert(std::string(otaStagingCodeName(
             OtaStagingCode::SignatureRejected)) == "SIGNATURE_REJECTED");
  assert(std::string(espOtaStagingCodeName(
             EspOtaStagingCode::TargetAliasesRunning)) ==
         "TARGET_ALIASES_RUNNING");
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-ota-staging-"));
  try {
    const source = join(scratch, "ota-staging.cpp");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(component, "include"), source,
      join(component, "ota_sha256.cpp"),
      join(component, "ota_staging.cpp"),
      join(component, "esp_ota_staging.cpp"),
      "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("bounded signed OTA core passes strict C++17 fault matrix", () => {
  run(false);
});

test("bounded signed OTA core passes ASan/UBSan fault matrix", () => {
  run(true);
});

test("ESP OTA staging source is isolated to exact inactive-slot APIs", () => {
  const system = readFileSync(
    join(component, "esp_ota_staging_system_api.cpp"), "utf8",
  );
  const adapter = readFileSync(
    join(component, "esp_ota_staging.cpp"), "utf8",
  );
  const portable = readFileSync(join(component, "ota_staging.cpp"), "utf8");
  for (const api of [
    "esp_ota_get_running_partition",
    "esp_ota_get_next_update_partition",
    "esp_ota_begin",
    "esp_ota_write",
    "esp_ota_end",
    "esp_ota_abort",
    "esp_ota_set_boot_partition",
  ]) assert.equal(system.split(api).length - 1, 1, api);
  assert.match(system, /esp_ota_get_next_update_partition\(nullptr\)/);
  assert.doesNotMatch(
    system + adapter,
    /esp_ota_get_boot_partition|esp_ota_resume|esp_ota_write_with_offset|esp_partition_(?:write|erase)|nvs_|littlefs|spiffs|sdmmc|coredump/i,
  );
  assert.doesNotMatch(
    system + adapter,
    /https?:\/\/|redirect|url|bearer|token|certificate|retry|download|reboot/i,
  );
  assert.doesNotMatch(
    portable,
    /#include\s*[<"](?:esp_|freertos|nvs|Arduino)|app_main|MyAI|Wi-?Fi|cloud/i,
  );
  const cmake = readFileSync(join(component, "CMakeLists.txt"), "utf8");
  for (const source of [
    "ota_sha256.cpp", "ota_staging.cpp", "esp_ota_staging.cpp",
    "esp_ota_staging_system_api.cpp",
  ]) assert.match(cmake, new RegExp(source.replace(".", "\\.")));
  assert.match(cmake, /REQUIRES app_update/);
});

test("ESP-IDF defaults enable application rollback", () => {
  const defaults = readFileSync(
    join(repo, "firmware/inkloop-idf/sdkconfig.defaults"), "utf8",
  );
  assert.match(defaults, /^CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y$/m);
  assert.equal(
    defaults.match(/^CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=/gm)?.length,
    1,
  );
});
