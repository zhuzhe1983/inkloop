#include "inkloop/settings/device_settings.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace inkloop {
namespace settings {
namespace {

bool isContinuation(std::uint8_t value) {
  return (value & 0xC0U) == 0x80U;
}

bool validScalar(std::uint32_t value) {
  if (value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU))
    return false;
  // Unicode noncharacters are never useful in user-editable settings and can
  // create mismatches between the WebUI, JSON, and font layers.
  if ((value >= 0xFDD0U && value <= 0xFDEFU) ||
      (value & 0xFFFFU) == 0xFFFEU || (value & 0xFFFFU) == 0xFFFFU)
    return false;
  return true;
}

}  // namespace

bool operator==(const DeviceSettings& left, const DeviceSettings& right) {
  return left.volume_percent == right.volume_percent &&
      left.led_maximum_brightness_percent ==
          right.led_maximum_brightness_percent &&
      left.led_roles_swapped == right.led_roles_swapped &&
      left.voice_assistance_enabled == right.voice_assistance_enabled &&
      left.assistant_prompt == right.assistant_prompt &&
      left.aigc_prompt_template == right.aigc_prompt_template &&
      left.aigc_steps == right.aigc_steps &&
      left.negative_prompt == right.negative_prompt &&
      left.asset_storage_preference == right.asset_storage_preference &&
      left.default_render_strategy == right.default_render_strategy &&
      left.local_management_password_override ==
          right.local_management_password_override;
}

bool validUtf8Text(const std::string& value, std::size_t maximum_bytes,
                   bool empty_allowed) {
  if (value.size() > maximum_bytes || (!empty_allowed && value.empty()))
    return false;
  std::size_t at = 0;
  while (at < value.size()) {
    const std::uint8_t first = static_cast<std::uint8_t>(value[at]);
    if (first <= 0x7FU) {
      if (first == 0U || (first < 0x20U && first != '\t' && first != '\n' &&
                         first != '\r') || first == 0x7FU)
        return false;
      ++at;
      continue;
    }

    std::uint32_t scalar = 0;
    std::size_t trailing = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
      scalar = first & 0x1FU;
      trailing = 1U;
      minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      scalar = first & 0x0FU;
      trailing = 2U;
      minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      scalar = first & 0x07U;
      trailing = 3U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (trailing > value.size() - at - 1U) return false;
    for (std::size_t index = 1U; index <= trailing; ++index) {
      const std::uint8_t next = static_cast<std::uint8_t>(value[at + index]);
      if (!isContinuation(next)) return false;
      scalar = (scalar << 6U) | (next & 0x3FU);
    }
    if (scalar < minimum || !validScalar(scalar)) return false;
    at += trailing + 1U;
  }
  return true;
}

bool validRenderStrategyId(const std::string& value) {
  if (value.empty() || value.size() > kMaximumRenderStrategyBytes ||
      value.front() < 'a' || value.front() > 'z')
    return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.';
  });
}

bool validLocalManagementPasswordOverride(const std::string& value) {
  if (value.empty()) return true;
  if (value.size() < 8U ||
      value.size() > kMaximumLocalManagementPasswordBytes) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return ch >= 0x20U && ch <= 0x7eU;
  });
}

bool validDeviceSettings(const DeviceSettings& value) {
  const std::uint8_t storage =
      static_cast<std::uint8_t>(value.asset_storage_preference);
  return value.volume_percent <= 100U &&
      value.led_maximum_brightness_percent <= 100U && storage <=
          static_cast<std::uint8_t>(AssetStoragePreference::Removable) &&
      validUtf8Text(value.assistant_prompt, kMaximumAssistantPromptBytes,
                    false) &&
      validUtf8Text(value.aigc_prompt_template,
                    kMaximumAigcPromptTemplateBytes, false) &&
      value.aigc_steps >= kMinimumAigcSteps &&
      value.aigc_steps <= kMaximumAigcSteps &&
      validUtf8Text(value.negative_prompt, kMaximumNegativePromptBytes, true) &&
      validRenderStrategyId(value.default_render_strategy) &&
      validLocalManagementPasswordOverride(
          value.local_management_password_override);
}

DeviceSettings makeGenericDeviceDefaults() {
  DeviceSettings output;
  output.assistant_prompt =
      "You are Inkloop's concise device assistant. Use local tools only when "
      "needed and require confirmation before destructive operations.";
  output.aigc_prompt_template =
      "Create a clear, high-contrast image suitable for the selected display. "
      "Use bold shapes and a simple composition. Subject: {prompt}";
  output.negative_prompt =
      "tiny text, watermark, low contrast, muddy colors, clipped subject";
  return output;
}

SettingsStatus makePaperColorDefaults(
    std::uint16_t width, std::uint16_t height,
    const std::string& panel_description, DeviceSettings& output) {
  if (width == 0U || height == 0U || width > 4096U || height > 4096U ||
      !validUtf8Text(panel_description, 128U, false))
    return {SettingsError::InvalidArgument,
            "invalid PaperColor settings profile"};
  DeviceSettings candidate;
  const std::string geometry = std::to_string(width) + "x" +
      std::to_string(height);
  candidate.assistant_prompt =
      "你是 Inkloop 数字墨水屏上友好、简洁、有个性的语音助手。当前设备是 " +
      geometry + " " + panel_description +
      "，整屏刷新较慢，应避免无意义频繁刷屏。你可以调用本地工具管理相册、"
      "查询剩余空间、调整音量与提示词、生成图片；删除、清空或格式化前必须明确确认。";
  candidate.aigc_prompt_template =
      "为 " + geometry + " " + panel_description +
      " 创作适合电子纸展示的图片：色彩鲜艳、高对比、清晰轮廓、大色块、"
      "简洁构图、少用细小文字。主题：{prompt}";
  candidate.negative_prompt =
      "细小文字，水印，低对比，灰暗，复杂渐变，细碎纹理，主体超出边界";
  if (!validDeviceSettings(candidate))
    return {SettingsError::TooLarge, "PaperColor defaults exceed bounds"};
  output = std::move(candidate);
  return SettingsStatus::success();
}

const char* settingsErrorName(SettingsError value) {
  switch (value) {
    case SettingsError::None: return "none";
    case SettingsError::InvalidArgument: return "invalid_argument";
    case SettingsError::InvalidState: return "invalid_state";
    case SettingsError::Storage: return "storage";
    case SettingsError::Corrupt: return "corrupt";
    case SettingsError::Conflict: return "conflict";
    case SettingsError::Exhausted: return "exhausted";
    case SettingsError::TooLarge: return "too_large";
  }
  return "unknown";
}

const char* assetStoragePreferenceName(AssetStoragePreference value) {
  switch (value) {
    case AssetStoragePreference::Automatic: return "automatic";
    case AssetStoragePreference::Internal: return "internal";
    case AssetStoragePreference::Removable: return "removable";
  }
  return "unknown";
}

}  // namespace settings
}  // namespace inkloop
