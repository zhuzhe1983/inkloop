#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <lwip/sockets.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "AlbumStore.h"
#include "MyAiAdapters.h"
#include "MyAiIngressPrimitives.h"
#include "VoiceAdapters.h"

namespace inkloop {

class PaperColorClock final : public myai::IClock, public voice::IClock {
 public:
  uint64_t monotonicMs() const override;
  std::string utcIso8601() const override;
};

class Esp32PublicEndpointSecurity final : public myai::IEndpointSecurity {
 public:
  myai::Status validatePublicTlsEndpoint(const std::string& httpsUrl) override;

  static bool parsePublicTlsUrl(const std::string& url, std::string& host,
                                uint16_t& port, std::string* path = nullptr);
  static bool publicAddress(const sockaddr* address);
};

class Esp32HttpsTransport final : public myai::IHttpTransport {
 public:
  myai::Status perform(const myai::HttpRequest& request,
                       myai::HttpResponse& response) override;

 private:
  static myai::Status readBounded(HTTPClient& http, size_t maximum,
                                  std::string& output);
};

// Two complete NVS slots are used so power loss can never turn a partially
// written device credential into a valid state. The highest valid generation
// wins; corruption fails closed instead of silently re-pairing.
class NvsMyAiCredentialStore final : public myai::ICredentialStore {
 public:
  myai::Status load(myai::CredentialSnapshot& snapshot) override;
  myai::Status initializeFingerprintAtomically(
      const std::string& installationFingerprint) override;
  myai::Status savePendingAtomically(
      const myai::PendingPairing& pending) override;
  myai::Status promoteBoundAtomically(
      const std::string& expectedPairingToken,
      const std::string& deviceId,
      const std::string& deviceToken,
      bool active) override;
  myai::Status clearPendingAtomically() override;
  myai::Status clearRuntimeCredentialAtomically() override;

 private:
  myai::Status store(const myai::CredentialSnapshot& snapshot);
  static bool encode(const myai::CredentialSnapshot& snapshot,
                     std::string& output);
  static bool decode(const std::string& input,
                     myai::CredentialSnapshot& snapshot);
};

class Esp32MyAiWebSocket final : public myai::IWebSocketTransport {
 public:
  Esp32MyAiWebSocket();

  myai::Status connect(
      const std::string& url,
      const std::map<std::string, std::string>& headers,
      myai::IWebSocketListener& listener) override;
  myai::Status sendText(const std::string& message) override;
  myai::Status sendBinary(const uint8_t* bytes, size_t length) override;
  void close(uint16_t code, const std::string& reason) override;

  void loop();
  bool connected() const { return connected_; }

 private:
  void onEvent(WStype_t type, uint8_t* payload, size_t length);
  void rejectIngress(
      myai::IWebSocketListener* listener, uint16_t code,
      const char* safeReason);

  WebSocketsClient socket_;
  myai::IWebSocketListener* listener_ = nullptr;
  std::string extraHeaders_;
  bool connected_ = false;
};

// Streaming TTS is staged by MyAiClient::IAudioSink::begin. The integration
// explicitly authorizes the speaker only after VoiceRuntime has proved the
// microphone and packaged prompts are quiescent.
class PaperColorStreamingAudio final : public myai::IAudioSink {
 public:
  myai::Status begin(uint32_t sampleRateHz, uint8_t channels) override;
  myai::Status write(const uint8_t* bytes, size_t length) override;
  myai::Status end() override;
  void abort() override;

  myai::Status authorize();
  bool active() const { return pending_ || authorized_ || M5.Speaker.isPlaying(); }
  void setVolume(uint8_t percent);
  void setEndedCallback(const std::function<void()>& callback) {
    endedCallback_ = callback;
  }

 private:
  bool waitForPlayback(uint32_t timeoutMs);

  uint32_t sampleRateHz_ = 16000;
  uint8_t channels_ = 1;
  uint8_t volumePercent_ = 60;
  bool pending_ = false;
  bool authorized_ = false;
  std::vector<int16_t> playback_;
  std::function<void()> endedCallback_;
};

// The JSON envelope is parsed incrementally and base64 is decoded directly
// into the bounded sink. No complete response or encoded image is retained.
class Esp32AigcOutputTransport final : public myai::IAigcOutputTransport {
 public:
  myai::Status postAndDecodeBase64(
      const myai::HttpRequest& request,
      size_t maxEncodedBytes,
      size_t maxDecodedBytes,
      myai::IImageSink& sink,
      myai::AigcOutputMetadata& metadata) override;
};

// AlbumStore already owns transactional asset/index promotion. This sink only
// provides a bounded PSRAM staging buffer, then delegates the durable commit.
class AlbumImageSink final : public myai::IImageSink {
 public:
  explicit AlbumImageSink(AlbumStore& album) : album_(album) {}
  ~AlbumImageSink() override { abort(); }

  myai::Status begin(const myai::AigcOutputMetadata& metadata) override;
  myai::Status write(const uint8_t* bytes, size_t length) override;
  myai::Status commit(myai::AigcOutputMetadata& metadata) override;
  void abort() override;

  void setMaximumBytes(size_t value) { maximumBytes_ = value; }
  bool takeCommittedAsset(AlbumAsset& asset);

 private:
  AlbumStore& album_;
  uint8_t* bytes_ = nullptr;
  size_t length_ = 0;
  size_t maximumBytes_ = 3U * 1024U * 1024U;
  AlbumAsset committed_;
  bool hasCommitted_ = false;
};

}  // namespace inkloop
