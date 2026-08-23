#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace inkloop {

inline bool voiceAigcContainsAny(
    const std::string& text,
    std::initializer_list<const char*> values) {
  for (const char* value : values) {
    if (value && text.find(value) != std::string::npos) return true;
  }
  return false;
}

inline std::string normalizedVoiceAigcText(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(
                       ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
                 });
  const std::string ascii_punctuation = " \t\r\n,.;:!?";
  const size_t begin = text.find_first_not_of(ascii_punctuation);
  if (begin == std::string::npos) return std::string();
  const size_t end = text.find_last_not_of(ascii_punctuation);
  text = text.substr(begin, end - begin + 1U);
  bool stripped = true;
  while (stripped && !text.empty()) {
    stripped = false;
    for (const char* suffix : {"，", "。", "；", "：", "！", "？"}) {
      const std::string marker(suffix);
      if (text.size() >= marker.size() &&
          text.compare(text.size() - marker.size(), marker.size(), marker) ==
              0) {
        text.erase(text.size() - marker.size());
        stripped = true;
        break;
      }
    }
  }
  return text;
}

inline bool voiceAigcExplicitRejection(const std::string& text) {
  const std::string normalized = normalizedVoiceAigcText(text);
  return voiceAigcContainsAny(
      normalized,
      {"不要生成", "不要画", "不要绘制", "不生成图片", "不用了",
       "算了", "取消", "停止", "do not generate", "don't generate",
       "dont generate", "not generate", "do not draw", "don't draw",
       "cancel", "never mind", "stop"});
}

inline bool voiceAigcExplicitIntent(const std::string& text) {
  if (text.empty() || text.size() > 1024U ||
      voiceAigcExplicitRejection(text)) {
    return false;
  }
  const std::string normalized = normalizedVoiceAigcText(text);
  if (voiceAigcContainsAny(
          normalized,
          {"什么是", "为什么", "怎么", "如何", "解释", "介绍",
           "what is", "why ", "how to", "explain "})) {
    return false;
  }
  if (voiceAigcContainsAny(
          normalized,
          {"帮我生成", "请生成", "给我生成", "我要生成", "想生成",
           "画一", "画个", "画一个", "帮我画", "请画", "给我画",
           "来一张", "做一张", "换一张", "draw a ", "draw an ",
           "paint a ", "paint an "})) {
    return true;
  }
  const bool chinese_action = voiceAigcContainsAny(
      normalized, {"生成", "绘制", "创作", "制作", "设计"});
  const bool chinese_visual = voiceAigcContainsAny(
      normalized, {"图片", "图像", "一张图", "一幅图", "插画", "海报",
                   "壁纸", "照片", "素材", "卡片", "屏保", "画面"});
  if (chinese_action && chinese_visual) return true;
  const bool english_action = voiceAigcContainsAny(
      normalized, {"generate", "draw", "create", "make", "design",
                   "render"});
  const bool english_visual = voiceAigcContainsAny(
      normalized, {"image", "picture", "illustration", "poster",
                   "wallpaper", "photo", "artwork", "card",
                   "screensaver"});
  return english_action && english_visual;
}

inline bool voiceAigcShortConfirmation(const std::string& text) {
  const std::string normalized = normalizedVoiceAigcText(text);
  if (normalized.empty() || normalized.size() > 32U) return false;
  for (const char* value : {"好", "好的", "可以", "需要", "确认", "是",
                            "是的", "对", "要", "生成吧", "画吧", "继续",
                            "yes", "yes please", "ok", "okay", "sure",
                            "confirm", "do it", "continue"}) {
    if (normalized == value) return true;
  }
  return false;
}

// A short affirmative is authority only while a prior explicit image request
// remains armed. It can never arm a fresh tool call by itself.
inline bool nextVoiceAigcIntentArmed(bool currently_armed,
                                    bool confirmation_window_open,
                                    const std::string& transcript) {
  if (voiceAigcExplicitRejection(transcript)) return false;
  if (voiceAigcExplicitIntent(transcript)) return true;
  return currently_armed && confirmation_window_open &&
         voiceAigcShortConfirmation(transcript);
}

// Correlate an authenticated action.execute with the exact final ASR turn
// that armed it. This is not a credential or a cryptographic authenticator;
// the WSS lease provides authenticity. The bounded fingerprint prevents a
// delayed or unrelated action on that lease from spending another turn's
// one-shot local authority without retaining user text in shared state.
inline uint64_t voiceAigcRequestFingerprint(const std::string& text) {
  const std::string normalized = normalizedVoiceAigcText(text);
  if (normalized.empty() || normalized.size() > 1024U) return 0U;
  uint64_t fingerprint = 14695981039346656037ULL;
  for (const unsigned char byte : normalized) {
    fingerprint ^= static_cast<uint64_t>(byte);
    fingerprint *= 1099511628211ULL;
  }
  fingerprint ^= static_cast<uint64_t>(normalized.size());
  return fingerprint == 0U ? 1U : fingerprint;
}

// Center may attach action.execute.original_request to either the original
// explicit image request or the latest short confirmation (for example
// “好的”). Keep both bounded fingerprints while the same one-shot authority
// window is open. A new explicit request replaces the pair; a rejection,
// unrelated utterance or expired window clears it.
struct VoiceAigcIntentCorrelation {
  bool armed = false;
  uint64_t explicit_request = 0U;
  uint64_t latest_utterance = 0U;
};

inline VoiceAigcIntentCorrelation nextVoiceAigcIntentCorrelation(
    const VoiceAigcIntentCorrelation& current,
    bool confirmation_window_open, const std::string& transcript) {
  VoiceAigcIntentCorrelation output;
  if (voiceAigcExplicitRejection(transcript)) return output;
  const uint64_t fingerprint = voiceAigcRequestFingerprint(transcript);
  if (fingerprint == 0U) return output;
  if (voiceAigcExplicitIntent(transcript)) {
    output.armed = true;
    output.explicit_request = fingerprint;
    output.latest_utterance = fingerprint;
    return output;
  }
  if (current.armed && confirmation_window_open &&
      current.explicit_request != 0U &&
      voiceAigcShortConfirmation(transcript)) {
    output.armed = true;
    output.explicit_request = current.explicit_request;
    output.latest_utterance = fingerprint;
  }
  return output;
}

inline bool voiceAigcIntentCorrelationMatches(
    const VoiceAigcIntentCorrelation& correlation,
    uint64_t action_request_fingerprint) {
  return correlation.armed && action_request_fingerprint != 0U &&
      (action_request_fingerprint == correlation.explicit_request ||
       action_request_fingerprint == correlation.latest_utterance);
}

}  // namespace inkloop
