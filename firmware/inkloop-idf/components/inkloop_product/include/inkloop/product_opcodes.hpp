#pragma once

#include <cstdint>

namespace inkloop {

// Stable on-device operation identifiers. WorkEnvelope remains generic and
// bounded; product owners interpret only the opcodes routed to their lane.
enum class ProductOpcode : uint16_t {
  None = 0,
  RawButtonPrevious = 1,
  RawButtonNext = 2,
  RawButtonVoice = 3,
  VoiceTopButton = 10,
  VoiceStartCapture = 11,
  VoiceStateChanged = 12,
  VoiceApplyVolume = 13,
  VoicePreviewVolume = 14,
  VoiceApplyAssistance = 15,
  VoicePromptOrdinal = 20,
  VoicePromptRefreshOrdinal = 21,
  VoicePromptPleaseWait = 22,
  VoicePromptAlbumEmpty = 23,
  VoicePromptDeviceRestored = 24,
  // Additional fixed, offline feedback selected by LocalPrompt in flags.
  // Tool execution remains on Portal; only bounded prompt playback crosses
  // to the responsive Voice owner.
  VoicePromptToolStatus = 25,
  SetVoiceLed = 100,
  SetImageLed = 101,
  SetLedMaximumBrightness = 102,
  NetworkVoiceBegin = 200,
  NetworkVoiceCancel = 201,
  NetworkApplySystemPrompt = 202,
  NetworkStartMyAiPairing = 203,
  NetworkRebindMyAi = 204,
  NetworkQueueAigc = 205,
  StorageAppendChat = 300,
  StorageReadLocalChat = 301,
  StorageClearLocalChat = 302,
  StorageRecoverLocalChatAfterFormat = 303,
  StorageSetTutorialState = 304,
  PortalRunAigc = 400,
  PortalRunLocalTool = 401,
  PortalConfirmLocalTool = 402,
  DisplayAlbumOrdinal = 500,
  AlbumRefreshStarting = 501,
  // Identical display path with a credential-free completion event for the
  // physical serial acceptance harness.
  DisplayDiagnosticAigcOrdinal = 502,
};

inline constexpr uint16_t productOpcode(ProductOpcode value) {
  return static_cast<uint16_t>(value);
}

}  // namespace inkloop
