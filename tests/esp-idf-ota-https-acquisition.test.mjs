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
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "inkloop/ota_https_acquisition.hpp"

using namespace inkloop;

static OtaTextView text(const std::string& value) {
  return {value.data(), value.size()};
}

static OtaBytesView bytes(const std::string& value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

static OtaSha256Digest digestOf(const std::vector<std::uint8_t>& value) {
  OtaSha256 hash;
  assert(hash.update(value.data(), value.size()));
  OtaSha256Digest output{};
  assert(hash.finish(output));
  return output;
}

template <typename Container>
static std::string lowerHex(const Container& value) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const std::uint8_t byte : value)
    output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

struct Fixture {
  std::string manifest_url = "https://updates.example.com/ota/c151.json";
  std::string image_url = "https://updates.example.com/ota/c151-1.3.0.bin";
  std::string board = "papercolor-esp32s3";
  std::string current = "1.2.3";
  std::string target = "1.3.0";
  std::vector<std::uint8_t> image;
  OtaSha256Digest digest{};
  std::array<std::uint8_t, kEd25519DetachedSignatureBytes> signature{};

  Fixture() {
    image.resize(9201U);
    for (std::size_t at = 0U; at < image.size(); ++at)
      image[at] = static_cast<std::uint8_t>((at * 29U + 7U) & 0xFFU);
    digest = digestOf(image);
    signature.fill(0x5AU);
  }

  std::string manifest(
      const std::string& board_override = std::string(),
      const std::string& version_override = std::string(),
      const std::string& url_override = std::string(),
      std::uint64_t size_override = std::numeric_limits<std::uint64_t>::max(),
      const std::string& digest_override = std::string(),
      const std::string& policy_override = std::string(),
      const std::string& signature_override = std::string(),
      std::uint64_t schema_override = 1U) const {
    const std::string& used_board =
        board_override.empty() ? board : board_override;
    const std::string& used_version =
        version_override.empty() ? target : version_override;
    const std::string& used_url =
        url_override.empty() ? image_url : url_override;
    const std::uint64_t used_size =
        size_override == std::numeric_limits<std::uint64_t>::max()
            ? image.size() : size_override;
    const std::string used_digest =
        digest_override.empty() ? lowerHex(digest) : digest_override;
    const std::string used_policy = policy_override.empty()
        ? kPinnedEd25519Sha256Policy : policy_override;
    const std::string used_signature = signature_override.empty()
        ? lowerHex(signature) : signature_override;
    std::ostringstream output;
    output << "{\"schema_version\":" << schema_override
           << ",\"board_sku\":\"" << used_board
           << "\",\"firmware_version\":\"" << used_version
           << "\",\"image_url\":\"" << used_url
           << "\",\"image_size\":" << used_size
           << ",\"image_sha256\":\"" << used_digest
           << "\",\"signature_policy\":\"" << used_policy
           << "\",\"detached_signature\":\"" << used_signature
           << "\"}";
    return output.str();
  }
};

static std::string replaceOne(std::string value, const std::string& from,
                              const std::string& to) {
  const std::size_t at = value.find(from);
  assert(at != std::string::npos);
  value.replace(at, from.size(), to);
  return value;
}

static OtaManifestParseCode parse(const Fixture& fixture,
                                  const std::string& document,
                                  AcquiredOtaManifest* parsed = nullptr,
                                  const std::string& current = std::string()) {
  AcquiredOtaManifest local;
  const OtaManifestParseCode code = parseOtaManifestDocument(
      bytes(document), text(fixture.board),
      text(current.empty() ? fixture.current : current), local);
  if (parsed) *parsed = local;
  return code;
}

class TestVerifier final : public IPinnedOtaSignatureVerifier {
 public:
  bool supportsPolicy(OtaTextView policy) const override {
    const std::string expected = kPinnedEd25519Sha256Policy;
    return supports && policy.data && policy.length == expected.size() &&
        std::memcmp(policy.data, expected.data(), policy.length) == 0;
  }

  bool verify(OtaTextView, const CanonicalOtaManifest&,
              const OtaSha256Digest&, OtaBytesView signature) const override {
    if (!accepts || !signature.data ||
        signature.length != kEd25519DetachedSignatureBytes)
      return false;
    for (std::size_t at = 0U; at < signature.length; ++at) {
      if (signature.data[at] != 0x5AU) return false;
    }
    return true;
  }

  bool supports = true;
  bool accepts = true;
};

struct FakeEsp {
  int running_tag = 1;
  int target_tag = 2;
  std::uint64_t capacity = kMaximumOtaImageBytes;
  int fail_write_call = 0;
  int running_calls = 0;
  int target_calls = 0;
  int begin_calls = 0;
  int write_calls = 0;
  int end_calls = 0;
  int abort_calls = 0;
  int select_calls = 0;
  std::size_t begin_size = 0U;
  std::uint64_t written = 0U;
};

static FakeEsp g_esp;

static void resetEsp() { g_esp = FakeEsp{}; }
static EspOtaPartition runningPartition() {
  ++g_esp.running_calls;
  return &g_esp.running_tag;
}
static EspOtaPartition nextPartition() {
  ++g_esp.target_calls;
  return &g_esp.target_tag;
}
static std::uint64_t partitionCapacity(EspOtaPartition partition) {
  assert(partition == &g_esp.target_tag);
  return g_esp.capacity;
}
static int beginWrite(EspOtaPartition partition, std::size_t size,
                      EspOtaHandle& handle) {
  assert(partition == &g_esp.target_tag);
  ++g_esp.begin_calls;
  g_esp.begin_size = size;
  handle = 0x3313U;
  return 0;
}
static int writeChunk(EspOtaHandle handle, const std::uint8_t*,
                      std::size_t length) {
  assert(handle == 0x3313U);
  ++g_esp.write_calls;
  if (g_esp.fail_write_call == g_esp.write_calls) return 71;
  g_esp.written += length;
  return 0;
}
static int endWrite(EspOtaHandle handle) {
  assert(handle == 0x3313U);
  ++g_esp.end_calls;
  return 0;
}
static int abortWrite(EspOtaHandle handle) {
  assert(handle == 0x3313U);
  ++g_esp.abort_calls;
  return 0;
}
static int selectPartition(EspOtaPartition partition) {
  assert(partition == &g_esp.target_tag);
  ++g_esp.select_calls;
  return 0;
}

static const EspOtaWriterFunctions kWriterFunctions{
    &runningPartition, &nextPartition, &partitionCapacity, &beginWrite,
    &writeChunk, &endWrite, &abortWrite, &selectPartition};

class FakeClock final : public IOtaMonotonicClock {
 public:
  std::uint64_t nowMs() const override { return now; }
  std::uint64_t now = 5000U;
};

struct FakeResponse {
  std::string url;
  std::vector<std::uint8_t> body;
  OtaHttpsFetchCode direct_code = OtaHttpsFetchCode::Ok;
  std::uint64_t declared_length =
      std::numeric_limits<std::uint64_t>::max();
  std::size_t chunk_bytes = 997U;
  bool advance_to_deadline = false;
  bool bypass_length_contract = false;
};

class FakeTransport final : public IOtaHttpsTransport {
 public:
  explicit FakeTransport(FakeClock& clock) : clock_(clock) {}

  OtaHttpsFetchObservation get(const OtaHttpsFetchRequest& request,
                               IOtaHttpsBodySink& sink) override {
    assert(next_ < responses.size());
    const FakeResponse& response = responses[next_++];
    assert(request.url.data &&
           std::string(request.url.data, request.url.length) == response.url);
    deadlines.push_back(request.deadline_ms);
    expected_lengths.push_back(request.expected_content_length);
    maximum_lengths.push_back(request.maximum_content_length);
    if (response.direct_code != OtaHttpsFetchCode::Ok) {
      OtaHttpsFetchObservation failed;
      failed.code = response.direct_code;
      return failed;
    }
    if (response.advance_to_deadline) {
      clock_.now = request.deadline_ms;
      OtaHttpsFetchObservation failed;
      failed.code = OtaHttpsFetchCode::DeadlineExceeded;
      return failed;
    }
    const std::uint64_t declared = response.declared_length ==
        std::numeric_limits<std::uint64_t>::max()
        ? response.body.size() : response.declared_length;
    if (!response.bypass_length_contract) {
      if (declared == 0U) {
        OtaHttpsFetchObservation failed;
        failed.code = OtaHttpsFetchCode::ContentLengthRequired;
        return failed;
      }
      if (declared > request.maximum_content_length) {
        OtaHttpsFetchObservation failed;
        failed.code = OtaHttpsFetchCode::ResponseTooLarge;
        return failed;
      }
      if (request.expected_content_length != 0U &&
          declared != request.expected_content_length) {
        OtaHttpsFetchObservation failed;
        failed.code = OtaHttpsFetchCode::ContentLengthMismatch;
        return failed;
      }
    }
    std::size_t at = 0U;
    while (at < response.body.size()) {
      const std::size_t count = std::min(
          std::min(response.chunk_bytes, request.maximum_chunk_bytes),
          response.body.size() - at);
      if (!sink.append(response.body.data() + at, count)) {
        OtaHttpsFetchObservation failed;
        failed.code = OtaHttpsFetchCode::SinkRejected;
        failed.content_length = declared;
        failed.bytes_received = at;
        return failed;
      }
      at += count;
    }
    OtaHttpsFetchObservation output;
    output.content_length = declared;
    output.bytes_received = response.body.size();
    if (!response.bypass_length_contract && response.body.size() < declared)
      output.code = OtaHttpsFetchCode::Truncated;
    else if (!response.bypass_length_contract &&
             response.body.size() > declared)
      output.code = OtaHttpsFetchCode::ContentLengthMismatch;
    else
      output.code = OtaHttpsFetchCode::Ok;
    return output;
  }

  std::vector<FakeResponse> responses;
  std::vector<std::uint64_t> deadlines;
  std::vector<std::uint64_t> expected_lengths;
  std::vector<std::uint64_t> maximum_lengths;

 private:
  FakeClock& clock_;
  std::size_t next_ = 0U;
};

static FakeResponse response(const std::string& url,
                             const std::string& body) {
  FakeResponse output;
  output.url = url;
  output.body.assign(body.begin(), body.end());
  return output;
}

static FakeResponse response(const std::string& url,
                             const std::vector<std::uint8_t>& body) {
  FakeResponse output;
  output.url = url;
  output.body = body;
  return output;
}

static OtaHttpsAcquisitionConfig config(const Fixture& fixture) {
  return {text(fixture.manifest_url), text(fixture.board),
          text(fixture.current), 100000U};
}

static OtaHttpsAcquisitionObservation attempt(
    const Fixture& fixture, const std::string& manifest,
    FakeResponse image_response, TestVerifier* verifier) {
  resetEsp();
  FakeClock clock;
  FakeTransport transport(clock);
  transport.responses.push_back(response(fixture.manifest_url, manifest));
  transport.responses.push_back(std::move(image_response));
  EspOtaStagingAdapter staging(kWriterFunctions, verifier);
  OtaHttpsAcquisition acquisition(clock, transport, staging);
  return acquisition.run(config(fixture));
}

static void urlPolicyMatrix() {
  ParsedOtaHttpsUrl standard;
  ParsedOtaHttpsUrl explicit_port;
  assert(parseOtaHttpsUrl(
      text(std::string("https://updates.example.com/a.bin")), standard) ==
      OtaHttpsUrlCode::Ok);
  assert(parseOtaHttpsUrl(
      text(std::string("https://updates.example.com:443/b.bin")),
      explicit_port) == OtaHttpsUrlCode::Ok);
  assert(sameOtaHttpsOrigin(standard, explicit_port));
  ParsedOtaHttpsUrl other;
  assert(parseOtaHttpsUrl(
      text(std::string("https://cdn.example.com/a.bin")), other) ==
      OtaHttpsUrlCode::Ok);
  assert(!sameOtaHttpsOrigin(standard, other));
  assert(parseOtaHttpsUrl(
      text(std::string("https://updates.example.com:444/a.bin")), other) ==
      OtaHttpsUrlCode::Ok);
  assert(!sameOtaHttpsOrigin(standard, other));
  assert(parseOtaHttpsUrl(text(std::string("http://updates.example.com/a")),
                          other) == OtaHttpsUrlCode::NonHttps);
  assert(parseOtaHttpsUrl(
      text(std::string("https://user:secret@updates.example.com/a")), other) ==
      OtaHttpsUrlCode::CredentialsRejected);
  assert(parseOtaHttpsUrl(
      text(std::string("https://updates.example.com/a?token=secret")), other) ==
      OtaHttpsUrlCode::InvalidPath);
  assert(parseOtaHttpsUrl(
      text(std::string("https://updates.example.com/a#fragment")), other) ==
      OtaHttpsUrlCode::InvalidPath);
  assert(parseOtaHttpsUrl(text(std::string("https://127.0.0.1/a")), other) ==
      OtaHttpsUrlCode::InvalidHost);
  assert(parseOtaHttpsUrl(text(std::string("https://localhost/a")), other) ==
      OtaHttpsUrlCode::InvalidHost);
  assert(parseOtaHttpsUrl(
      text(std::string("https://Updates.example.com/a")), other) ==
      OtaHttpsUrlCode::InvalidHost);
  assert(parseOtaHttpsUrl(
      text(std::string("https://updates.example.com/a b")), other) ==
      OtaHttpsUrlCode::InvalidPath);
}

static void strictManifestMatrix() {
  Fixture fixture;
  AcquiredOtaManifest parsed;
  assert(parse(fixture, fixture.manifest(), &parsed) ==
         OtaManifestParseCode::Ok);
  assert(parsed.image_size == fixture.image.size());
  assert(parsed.image_sha256 == fixture.digest);
  assert(parsed.detached_signature == fixture.signature);

  std::string unknown = fixture.manifest();
  unknown.insert(unknown.size() - 1U, ",\"unknown\":1");
  assert(parse(fixture, unknown) == OtaManifestParseCode::UnknownField);
  std::string duplicate = fixture.manifest();
  duplicate.insert(duplicate.size() - 1U,
                   ",\"board_sku\":\"papercolor-esp32s3\"");
  assert(parse(fixture, duplicate) == OtaManifestParseCode::DuplicateField);
  const std::string missing = replaceOne(
      fixture.manifest(), ",\"image_size\":9201", "");
  assert(parse(fixture, missing) == OtaManifestParseCode::MissingField);
  assert(parse(fixture, "{") == OtaManifestParseCode::MalformedJson);
  std::string trailing = fixture.manifest();
  trailing.insert(trailing.size() - 1U, ",");
  assert(parse(fixture, trailing) == OtaManifestParseCode::MalformedJson);
  assert(parse(fixture, fixture.manifest(
      "", "", "", std::numeric_limits<std::uint64_t>::max(), "", "", "",
      2U)) == OtaManifestParseCode::InvalidSchema);
  assert(parse(fixture, fixture.manifest("other-board")) ==
         OtaManifestParseCode::InvalidBoard);
  assert(parse(fixture, fixture.manifest("", "1.2.3")) ==
         OtaManifestParseCode::TargetNotNewer);
  assert(parse(fixture, fixture.manifest("", "1.2.2")) ==
         OtaManifestParseCode::TargetNotNewer);
  assert(parse(fixture, fixture.manifest("", "1.03.0")) ==
         OtaManifestParseCode::InvalidVersion);
  assert(parse(fixture, fixture.manifest("", "1.3.0"), nullptr,
               "1.3.0+build.9") == OtaManifestParseCode::TargetNotNewer);
  assert(parse(fixture, fixture.manifest("", "1.3.0-rc.2"), nullptr,
               "1.3.0-rc.1") == OtaManifestParseCode::Ok);
  assert(parse(fixture, fixture.manifest("", "0.4.0-beta.2"), nullptr,
               "0.4.0-beta.1") == OtaManifestParseCode::Ok);
  assert(parse(fixture, fixture.manifest(
      "", "", "http://updates.example.com/image.bin")) ==
      OtaManifestParseCode::InvalidImageUrl);
  assert(parse(fixture, fixture.manifest(
      "", "", "https://user:secret@updates.example.com/image.bin")) ==
      OtaManifestParseCode::InvalidImageUrl);
  assert(parse(fixture, fixture.manifest(
      "", "", "", 0U)) == OtaManifestParseCode::InvalidImageSize);
  assert(parse(fixture, fixture.manifest(
      "", "", "", kMaximumOtaImageBytes + 1U)) ==
      OtaManifestParseCode::InvalidImageSize);
  std::string uppercase_digest = lowerHex(fixture.digest);
  uppercase_digest[0] = 'A';
  assert(parse(fixture, fixture.manifest(
      "", "", "", std::numeric_limits<std::uint64_t>::max(),
      uppercase_digest)) == OtaManifestParseCode::InvalidDigest);
  assert(parse(fixture, fixture.manifest(
      "", "", "", std::numeric_limits<std::uint64_t>::max(),
      std::string(64U, '0'))) == OtaManifestParseCode::InvalidDigest);
  assert(parse(fixture, fixture.manifest(
      "", "", "", std::numeric_limits<std::uint64_t>::max(), "",
      "other-policy")) == OtaManifestParseCode::InvalidSignaturePolicy);
  assert(parse(fixture, fixture.manifest(
      "", "", "", std::numeric_limits<std::uint64_t>::max(), "", "",
      std::string(126U, 'a'))) == OtaManifestParseCode::InvalidSignature);
  const std::string escaped = replaceOne(
      fixture.manifest(), "https://", "https:\\\\/\\\\/");
  assert(parse(fixture, escaped) == OtaManifestParseCode::MalformedJson);
  std::string oversized(kMaximumOtaManifestDocumentBytes + 1U, ' ');
  assert(parse(fixture, oversized) == OtaManifestParseCode::DocumentTooLarge);
  AcquiredOtaManifest empty;
  assert(parseOtaManifestDocument({nullptr, 1U}, text(fixture.board),
      text(fixture.current), empty) == OtaManifestParseCode::InvalidArgument);
}

static void successfulAcquisition() {
  Fixture fixture;
  TestVerifier verifier;
  resetEsp();
  FakeClock clock;
  FakeTransport transport(clock);
  transport.responses.push_back(response(
      fixture.manifest_url, fixture.manifest()));
  transport.responses.push_back(response(fixture.image_url, fixture.image));
  EspOtaStagingAdapter staging(kWriterFunctions, &verifier);
  OtaHttpsAcquisition acquisition(clock, transport, staging);
  const OtaHttpsAcquisitionObservation result =
      acquisition.run(config(fixture));
  assert(result.code == OtaHttpsAcquisitionCode::Ok);
  assert(result.deadline_ms == 105000U);
  assert(transport.deadlines.size() == 2U);
  assert(transport.deadlines[0] == transport.deadlines[1]);
  assert(transport.deadlines[0] == result.deadline_ms);
  assert(transport.expected_lengths[0] == 0U);
  assert(transport.maximum_lengths[0] ==
         kMaximumOtaManifestDocumentBytes);
  assert(transport.expected_lengths[1] == fixture.image.size());
  assert(transport.maximum_lengths[1] == fixture.image.size());
  assert(g_esp.begin_calls == 1 && g_esp.end_calls == 1);
  assert(g_esp.abort_calls == 0 && g_esp.select_calls == 1);
  assert(g_esp.begin_size == fixture.image.size());
  assert(g_esp.written == fixture.image.size());
  assert(staging.core().state() == OtaStagingState::BootSelected);
  assert(acquisition.run(config(fixture)).code ==
         OtaHttpsAcquisitionCode::InvalidState);
}

static void preMutationFailureMatrix() {
  Fixture fixture;
  TestVerifier verifier;
  auto invalidConfiguration = [&](const OtaHttpsAcquisitionConfig& invalid) {
    resetEsp();
    FakeClock clock;
    FakeTransport transport(clock);
    EspOtaStagingAdapter staging(kWriterFunctions, &verifier);
    OtaHttpsAcquisition acquisition(clock, transport, staging);
    assert(acquisition.run(invalid).code ==
           OtaHttpsAcquisitionCode::InvalidConfiguration);
    assert(g_esp.running_calls == 0 && g_esp.begin_calls == 0 &&
           g_esp.abort_calls == 0 && g_esp.select_calls == 0);
  };
  OtaHttpsAcquisitionConfig invalid = config(fixture);
  invalid.manifest_url = {};
  invalidConfiguration(invalid);
  invalid = config(fixture);
  invalid.total_deadline_ms = 0U;
  invalidConfiguration(invalid);
  invalid = config(fixture);
  invalid.total_deadline_ms = kMaximumOtaAcquisitionDeadlineMs + 1U;
  invalidConfiguration(invalid);
  const std::string invalid_version = "not-semver";
  invalid = config(fixture);
  invalid.current_firmware_version = text(invalid_version);
  invalidConfiguration(invalid);

  for (const OtaHttpsFetchCode code : {
           OtaHttpsFetchCode::RedirectRejected,
           OtaHttpsFetchCode::PeerRejected,
           OtaHttpsFetchCode::ContentLengthRequired,
           OtaHttpsFetchCode::ContentLengthMismatch,
           OtaHttpsFetchCode::DeadlineExceeded}) {
    resetEsp();
    FakeClock clock;
    FakeTransport transport(clock);
    FakeResponse manifest;
    manifest.url = fixture.manifest_url;
    manifest.direct_code = code;
    transport.responses.push_back(manifest);
    EspOtaStagingAdapter staging(kWriterFunctions, &verifier);
    OtaHttpsAcquisition acquisition(clock, transport, staging);
    const OtaHttpsAcquisitionObservation result =
        acquisition.run(config(fixture));
    assert(result.code == (code == OtaHttpsFetchCode::DeadlineExceeded
        ? OtaHttpsAcquisitionCode::DeadlineExceeded
        : OtaHttpsAcquisitionCode::ManifestFetchFailed));
    assert(g_esp.running_calls == 0 && g_esp.begin_calls == 0);
    assert(g_esp.abort_calls == 0 && g_esp.select_calls == 0);
  }

  OtaHttpsAcquisitionObservation result = attempt(
      fixture, fixture.manifest("", "",
          "https://cdn.example.com/image.bin"),
      response(fixture.image_url, fixture.image), &verifier);
  assert(result.code == OtaHttpsAcquisitionCode::ImageOriginMismatch);
  assert(g_esp.begin_calls == 0 && g_esp.abort_calls == 0);

  result = attempt(fixture, fixture.manifest("", "1.2.2"),
                   response(fixture.image_url, fixture.image), &verifier);
  assert(result.code == OtaHttpsAcquisitionCode::ManifestRejected);
  assert(result.manifest_code == OtaManifestParseCode::TargetNotNewer);
  assert(g_esp.begin_calls == 0 && g_esp.abort_calls == 0);

  result = attempt(fixture, fixture.manifest(),
                   response(fixture.image_url, fixture.image), nullptr);
  assert(result.code == OtaHttpsAcquisitionCode::StagingBeginFailed);
  assert(result.staging.code == EspOtaStagingCode::VerifierUnavailable);
  assert(g_esp.running_calls == 0 && g_esp.begin_calls == 0);
}

static void streamedFailureAndAbortMatrix() {
  Fixture fixture;
  TestVerifier verifier;
  for (const OtaHttpsFetchCode code : {
           OtaHttpsFetchCode::RedirectRejected,
           OtaHttpsFetchCode::PeerRejected,
           OtaHttpsFetchCode::ContentLengthMismatch,
           OtaHttpsFetchCode::DeadlineExceeded,
           OtaHttpsFetchCode::Truncated}) {
    FakeResponse image = response(fixture.image_url, fixture.image);
    image.direct_code = code;
    const OtaHttpsAcquisitionObservation result = attempt(
        fixture, fixture.manifest(), image, &verifier);
    assert(result.code == (code == OtaHttpsFetchCode::DeadlineExceeded
        ? OtaHttpsAcquisitionCode::DeadlineExceeded
        : OtaHttpsAcquisitionCode::ImageFetchFailed));
    assert(g_esp.begin_calls == 1 && g_esp.abort_calls == 1);
    assert(g_esp.select_calls == 0);
  }

  FakeResponse truncated = response(fixture.image_url, fixture.image);
  truncated.body.resize(fixture.image.size() - 17U);
  truncated.declared_length = fixture.image.size();
  OtaHttpsAcquisitionObservation result = attempt(
      fixture, fixture.manifest(), truncated, &verifier);
  assert(result.code == OtaHttpsAcquisitionCode::ImageFetchFailed);
  assert(result.fetch.code == OtaHttpsFetchCode::Truncated);
  assert(g_esp.written == fixture.image.size() - 17U);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);

  FakeResponse wrong_length = response(fixture.image_url, fixture.image);
  wrong_length.declared_length = fixture.image.size() - 1U;
  result = attempt(fixture, fixture.manifest(), wrong_length, &verifier);
  assert(result.fetch.code == OtaHttpsFetchCode::ContentLengthMismatch);
  assert(g_esp.write_calls == 0 && g_esp.abort_calls == 1);

  std::vector<std::uint8_t> overflow = fixture.image;
  overflow.push_back(0xEEU);
  FakeResponse overflow_response = response(fixture.image_url, overflow);
  overflow_response.bypass_length_contract = true;
  result = attempt(fixture, fixture.manifest(), overflow_response, &verifier);
  assert(result.code == OtaHttpsAcquisitionCode::ImageFetchFailed);
  assert(result.fetch.code == OtaHttpsFetchCode::SinkRejected);
  assert(result.staging.code == EspOtaStagingCode::ChunkRejected);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);

  resetEsp();
  g_esp.fail_write_call = 2;
  FakeClock clock;
  FakeTransport transport(clock);
  transport.responses.push_back(response(
      fixture.manifest_url, fixture.manifest()));
  transport.responses.push_back(response(fixture.image_url, fixture.image));
  EspOtaStagingAdapter staging(kWriterFunctions, &verifier);
  OtaHttpsAcquisition acquisition(clock, transport, staging);
  result = acquisition.run(config(fixture));
  assert(result.fetch.code == OtaHttpsFetchCode::SinkRejected);
  assert(result.staging.code == EspOtaStagingCode::WriteFailed);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);

  std::string wrong_digest = lowerHex(fixture.digest);
  wrong_digest[5] = wrong_digest[5] == '0' ? '1' : '0';
  result = attempt(fixture, fixture.manifest(
      "", "", "", std::numeric_limits<std::uint64_t>::max(),
      wrong_digest), response(fixture.image_url, fixture.image), &verifier);
  assert(result.code == OtaHttpsAcquisitionCode::StagingFinishFailed);
  assert(result.staging.code == EspOtaStagingCode::FinalizeRejected);
  assert(result.staging.core_code == OtaStagingCode::DigestMismatch);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);

  verifier.accepts = false;
  result = attempt(fixture, fixture.manifest(),
                   response(fixture.image_url, fixture.image), &verifier);
  assert(result.code == OtaHttpsAcquisitionCode::StagingFinishFailed);
  assert(result.staging.code == EspOtaStagingCode::SignatureRejected);
  assert(result.staging.core_code == OtaStagingCode::SignatureRejected);
  assert(g_esp.abort_calls == 1 && g_esp.select_calls == 0);
}

int main() {
  urlPolicyMatrix();
  strictManifestMatrix();
  successfulAcquisition();
  preMutationFailureMatrix();
  streamedFailureAndAbortMatrix();
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-ota-https-"));
  try {
    const source = join(scratch, "ota-https.cpp");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(component, "include"), source,
      join(component, "ota_https_acquisition.cpp"),
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

test("OTA HTTPS acquisition passes strict adversarial host matrix", () => {
  run(false);
});

test("OTA HTTPS acquisition passes ASan/UBSan matrix", () => {
  run(true);
});

test("ESP OTA HTTPS transport keeps the network boundary fail-closed", () => {
  const portable = readFileSync(
    join(component, "ota_https_acquisition.cpp"), "utf8",
  );
  const transport = readFileSync(
    join(component, "esp_ota_https_transport.cpp"), "utf8",
  );
  const header = readFileSync(join(
    component, "include/inkloop/ota_https_acquisition.hpp",
  ), "utf8");
  const cmake = readFileSync(join(component, "CMakeLists.txt"), "utf8");

  assert.match(transport, /#include "esp_crt_bundle\.h"/);
  assert.match(transport, /crt_bundle_attach = esp_crt_bundle_attach/);
  assert.match(transport, /skip_cert_common_name_check = false/);
  assert.match(transport, /disable_auto_redirect = true/);
  assert.match(transport, /max_redirection_count = 0/);
  assert.match(transport, /max_authorization_retries = -1/);
  assert.match(transport, /getpeername\(/);
  assert.match(transport, /connectedPeerPublic/);
  assert.match(transport, /esp_http_client_fetch_headers/);
  assert.match(transport, /ContentLengthRequired/);
  assert.match(transport, /ContentLengthMismatch/);
  assert.match(transport, /esp_http_client_is_complete_data_received/);
  assert.ok(
    (transport.match(/esp_http_client_set_timeout_ms/g) ?? []).length >= 2,
  );
  assert.match(portable, /sameOtaHttpsOrigin/);
  assert.match(portable, /staging_\.abort\(\)/);
  assert.match(header, /kMaximumOtaManifestDocumentBytes = 4096U/);
  assert.match(header, /kMaximumOtaHttpsTransportChunkBytes = 4096U/);
  assert.doesNotMatch(
    portable + transport,
    /ESP_LOG|printf|fprintf|"(?:Authorization|Bearer|password|token|api[_-]?key)/i,
  );
  assert.doesNotMatch(
    portable + transport,
    /esp_restart|esp_ota_set_boot_partition|esp_partition_(?:write|erase)|nvs_|fopen/,
  );
  assert.match(cmake, /"ota_https_acquisition\.cpp"/);
  assert.match(cmake, /"esp_ota_https_transport\.cpp"/);
  assert.match(cmake, /PRIV_REQUIRES esp_http_client esp-tls esp_timer lwip/);
});
