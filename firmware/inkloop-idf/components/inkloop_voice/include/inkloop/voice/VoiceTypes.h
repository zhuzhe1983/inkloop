#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace inkloop {
namespace voice {

enum class RuntimeState : uint8_t {
  Disabled,
  Ready,
  Listening,
  Thinking,
  Speaking,
  AwaitingSpokenConfirmation,
  AwaitingPhysicalConfirmation,
  Error,
};

enum class VoiceLedState : uint8_t { Off, Listening, Thinking, Speaking, Error };

enum class CommandKind : uint8_t {
  None,
  QueryFreeSpace,
  ListImages,
  SelectImage,
  DeleteImage,
  ClearAllImages,
  SetVolume,
  FormatStorage,
  SetAssistantPrompt,
  SetImageSetting,
  ResetTarget,
};

enum class ConfirmationStage : uint8_t { None, Spoken, Physical };

struct Status {
  bool success;
  std::string code;
  std::string detail;

  Status(bool ok = true, const std::string& errorCode = std::string(),
         const std::string& message = std::string())
      : success(ok), code(errorCode), detail(message) {}

  static Status ok() { return Status(); }
  static Status error(const std::string& code, const std::string& detail) {
    return Status(false, code, detail);
  }
};

struct ParsedCommand {
  CommandKind kind;
  std::string targetId;
  std::string key;
  std::string value;
  int number;
  bool english;

  ParsedCommand() : kind(CommandKind::None), number(0), english(false) {}
  bool matched() const { return kind != CommandKind::None; }
};

struct PendingConfirmation {
  ConfirmationStage stage;
  ParsedCommand command;
  std::string exactPhrase;
  std::string targetAlbumId;
  uint64_t targetRevision;
  uint32_t sessionGeneration;
  uint64_t expiresAtMs;

  PendingConfirmation()
      : stage(ConfirmationStage::None), targetRevision(0),
        sessionGeneration(0), expiresAtMs(0) {}
  bool active() const { return stage != ConfirmationStage::None; }
  void clear() {
    stage = ConfirmationStage::None;
    command = ParsedCommand();
    exactPhrase.clear();
    targetAlbumId.clear();
    targetRevision = 0;
    sessionGeneration = 0;
    expiresAtMs = 0;
  }
};

struct AlbumRevision {
  std::string albumId;
  uint64_t revision;

  AlbumRevision() : revision(0) {}
};

struct StorageSpace {
  uint64_t freeBytes;
  uint64_t totalBytes;
  std::string storageId;

  StorageSpace() : freeBytes(0), totalBytes(0) {}
};

struct ImageEntry {
  std::string id;
  std::string label;
  uint32_t ordinal;

  ImageEntry() : ordinal(0) {}
};

struct ImageSelection {
  std::string id;
  std::string albumId;
  std::string frameId;
  uint64_t revision;
  uint32_t ordinal;
  uint32_t total;

  ImageSelection() : revision(0), ordinal(0), total(0) {}
};

struct TranscriptDecision {
  bool handledLocally;
  bool awaitingConfirmation;
  std::string commandName;

  TranscriptDecision(bool handled = false, bool awaiting = false,
                     const std::string& name = std::string())
      : handledLocally(handled), awaitingConfirmation(awaiting),
        commandName(name) {}
};

inline const char* commandName(CommandKind kind) {
  switch (kind) {
    case CommandKind::QueryFreeSpace: return "storage.free";
    case CommandKind::ListImages: return "images.list";
    case CommandKind::SelectImage: return "images.select";
    case CommandKind::DeleteImage: return "images.delete";
    case CommandKind::ClearAllImages: return "images.clear_all";
    case CommandKind::SetVolume: return "audio.volume";
    case CommandKind::FormatStorage: return "storage.format";
    case CommandKind::SetAssistantPrompt: return "assistant.prompt";
    case CommandKind::SetImageSetting: return "image.setting";
    case CommandKind::ResetTarget: return "device.reset";
    default: return "none";
  }
}

}  // namespace voice
}  // namespace inkloop
