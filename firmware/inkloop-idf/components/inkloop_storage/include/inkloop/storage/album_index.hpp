#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace inkloop {
namespace storage {

inline constexpr size_t kMaximumAlbumEntries = 96U;
inline constexpr size_t kMaximumAlbumIndexBytes = 64U * 1024U;
inline constexpr size_t kMaximumAlbumAssetBytes = 1500000U;

enum class AlbumIndexCode : uint8_t {
  Ok,
  InvalidArgument,
  TooLarge,
  InvalidJson,
  InvalidSchema,
  InvalidAsset,
  DuplicateAsset,
  InvalidCurrent,
};

struct AlbumIndexAsset {
  // Logical entry id. Server tasks derive this from task id + content SHA so
  // two owners may safely share identical physical PNG bytes while retaining
  // independent render/delete semantics.
  std::string id;
  std::string path;
  // SHA-256 of the decoded file bytes and the physical content-address key.
  // Legacy schema-1 records infer this from id/path when omitted.
  std::string content_sha256;
  size_t bytes = 0;
  bool landscape = false;
  uint32_t created = 0;
  std::string task_id;
  std::string render_strategy = "official-quality";
};

struct AlbumIndex {
  std::string current;
  // Render strategy used for the pixels currently persisted on the panel.
  // Empty is accepted only as a legacy/unknown value and forces one refresh.
  std::string current_render_strategy;
  std::vector<AlbumIndexAsset> assets;
};

AlbumIndexCode parseAlbumIndex(const std::string& json, AlbumIndex& output);
AlbumIndexCode encodeAlbumIndex(const AlbumIndex& input, std::string& json);
bool validAlbumAssetId(const std::string& value);
bool validRenderStrategy(const std::string& value);
const char* albumIndexCodeName(AlbumIndexCode code);

}  // namespace storage
}  // namespace inkloop
