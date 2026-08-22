#include "inkloop/board_prompt_policy.hpp"

#include <cstring>

namespace inkloop {
namespace {

std::string boardName(const BoardDescriptor& board) {
  return board.id && board.id[0] != '\0' ? board.id : "inkloop-device";
}

std::string colorDescription(const BoardDescriptor& board) {
  if (board.palette_colors == 0U) return "数字墨水屏";
  return std::to_string(board.palette_colors) + " 色电子纸";
}

}  // namespace

std::string boardResolution(const BoardDescriptor& board) {
  return std::to_string(board.width) + "×" + std::to_string(board.height);
}

std::string boardPanelSummary(const BoardDescriptor& board) {
  return boardResolution(board) + "、" + colorDescription(board);
}

std::string defaultAssistantPrompt(const BoardDescriptor& board) {
  std::string prompt =
      "你是 Inkloop 数字墨水屏助手，正在管理 " + boardName(board) +
      "（" + boardPanelSummary(board) +
      "）。了解电子纸整屏刷新较慢，应避免无意义频繁刷屏。你可以调用本地"
      "工具管理相册、查询存储、生成图片和调整提示词；删除、清空或格式化前"
      "必须明确确认。";
  if (board.has_microphone && board.has_speaker)
    prompt += "设备支持语音对话和音量调整。";
  if (board.has_sd) prompt += "设备支持 TF 卡存储。";
  if (board.rgb_pixels > 0U) prompt += "设备支持状态灯反馈。";
  prompt += "不要声称设备具备未声明的能力，以友好、简洁、有个性的方式回应。";
  return prompt;
}

std::string defaultImagePromptTemplate(const BoardDescriptor& board) {
  return "适合 " + boardPanelSummary(board) +
      " 展示，鲜艳纯色，高对比度，清晰轮廓，主体明确，少渐变，无细小"
      "文字，构图严格适配目标画布：{prompt}";
}

std::string defaultNegativePrompt(const BoardDescriptor&) {
  return "细小文字，水印，低对比，灰暗，复杂渐变，细碎纹理，主体超出边界";
}

std::string myAiDeviceLabel(const BoardDescriptor& board) {
  return "Inkloop " + boardName(board);
}

std::string myAiInstallationFingerprintPrefix(const BoardDescriptor& board) {
  const std::string id = boardName(board);
  // Preserve the already-issued C151 credential namespace across the native
  // upgrade. New SKUs use their descriptor id directly and therefore require
  // no product-layer edits.
  if (id == "m5-papercolor-c151") return "papercolor-c151";
  return id;
}

std::string aigcImageSize(const BoardDescriptor& board) {
  return std::to_string(board.width) + "x" + std::to_string(board.height);
}

bool classifyBoardPngGeometry(const BoardDescriptor& board, uint32_t width,
                              uint32_t height, bool& landscape) {
  landscape = false;
  if (board.width == 0U || board.height == 0U) return false;
  if (width == board.width && height == board.height) return true;
  if (board.width != board.height && width == board.height &&
      height == board.width) {
    landscape = true;
    return true;
  }
  return false;
}

}  // namespace inkloop
