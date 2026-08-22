#include "inkloop/render/ImageProcessing.h"

#include <limits.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace inkloop {
namespace displaypower {

namespace {

// The reflectance-domain design and measured PaperColor ink anchors are
// adapted from MarsTechHAN/PaperColor-Frame at commit
// 304ac82a507ad59cc9fdfb4c82512543d7e21e1a (main/dither.c,
// main/color_pipeline.c, and main/palette.c). Upstream publishes GPL-3.0;
// keep the source attribution and distribution notice with firmware builds.

static const uint32_t kMaximumDimension = 8192;

bool samePixel(const RgbPixel& left, const RgbPixel& right) {
  return left.red == right.red && left.green == right.green && left.blue == right.blue;
}

uint64_t squaredDistance(const RgbPixel& left, const RgbPixel& right) {
  const int32_t red = static_cast<int32_t>(left.red) - right.red;
  const int32_t green = static_cast<int32_t>(left.green) - right.green;
  const int32_t blue = static_cast<int32_t>(left.blue) - right.blue;
  return static_cast<uint64_t>(red * red) + static_cast<uint64_t>(green * green) +
      static_cast<uint64_t>(blue * blue);
}

int32_t roundedDivide16(int32_t value) {
  return value >= 0 ? (value + 8) / 16 : (value - 8) / 16;
}

uint8_t clampByte(int32_t value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return static_cast<uint8_t>(value);
}

struct ErrorPixel {
  int32_t red;
  int32_t green;
  int32_t blue;

  ErrorPixel() : red(0), green(0), blue(0) {}
};

struct FloatColor {
  float first;
  float second;
  float third;

  FloatColor() : first(0.0f), second(0.0f), third(0.0f) {}
  FloatColor(float firstValue, float secondValue, float thirdValue)
      : first(firstValue), second(secondValue), third(thirdValue) {}
};

// PaperColor-Frame measured SCI anchors. Inkloop uses the measurements under
// a separately authorized integration and keeps the renderer bounded to three
// 400-pixel error rows rather than retaining a full float image in PSRAM.
static const FloatColor kMeasuredPaperColorLab[] = {
    FloatColor(30.6245f, 2.9592f, -6.3928f),
    FloatColor(65.4689f, -3.5446f, -1.2256f),
    FloatColor(62.8726f, -7.5112f, 45.0810f),
    FloatColor(35.6534f, 21.5614f, 8.6294f),
    FloatColor(40.6722f, -2.3424f, -24.7103f),
    FloatColor(41.0541f, -14.5846f, 11.8314f),
};

static const float kYuleNielsenN = 1.4f;

float clampFloat(float value, float minimum, float maximum) {
  return std::max(minimum, std::min(maximum, value));
}

float srgbToLinear(uint8_t encoded) {
  const float value = static_cast<float>(encoded) / 255.0f;
  return value <= 0.04045f
      ? value / 12.92f
      : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float labPivot(float value) {
  return value > 0.008856f ? std::cbrt(value) : 7.787f * value + 16.0f / 116.0f;
}

float inverseLabPivot(float value) {
  const float cube = value * value * value;
  return cube > 0.008856f ? cube : (value - 16.0f / 116.0f) / 7.787f;
}

FloatColor xyzToLab(const FloatColor& xyz) {
  const float x = labPivot(std::max(0.0f, xyz.first) / 0.95047f);
  const float y = labPivot(std::max(0.0f, xyz.second));
  const float z = labPivot(std::max(0.0f, xyz.third) / 1.08883f);
  return FloatColor(116.0f * y - 16.0f, 500.0f * (x - y), 200.0f * (y - z));
}

FloatColor labToXyz(const FloatColor& lab) {
  const float y = (lab.first + 16.0f) / 116.0f;
  const float x = y + lab.second / 500.0f;
  const float z = y - lab.third / 200.0f;
  return FloatColor(
      0.95047f * inverseLabPivot(x),
      inverseLabPivot(y),
      1.08883f * inverseLabPivot(z));
}

FloatColor sourceToPanelLab(const RgbPixel& source) {
  const float red = srgbToLinear(source.red);
  const float green = srgbToLinear(source.green);
  const float blue = srgbToLinear(source.blue);
  const FloatColor sourceLab = xyzToLab(FloatColor(
      red * 0.4124564f + green * 0.3575761f + blue * 0.1804375f,
      red * 0.2126729f + green * 0.7151522f + blue * 0.0721750f,
      red * 0.0193339f + green * 0.1191920f + blue * 0.9503041f));
  const float normalizedLightness = clampFloat(sourceLab.first / 100.0f, 0.0f, 1.0f);
  const float lifted = std::pow(normalizedLightness, 0.80f);
  const FloatColor& black = kMeasuredPaperColorLab[0];
  const FloatColor& white = kMeasuredPaperColorLab[1];
  return FloatColor(
      black.first + (white.first - black.first) * lifted,
      sourceLab.second * 0.44f,
      sourceLab.third * 0.44f);
}

FloatColor labToPseudoReflectance(const FloatColor& lab) {
  const FloatColor xyz = labToXyz(lab);
  const float inverseN = 1.0f / kYuleNielsenN;
  return FloatColor(
      std::pow(std::max(0.0f, xyz.first), inverseN),
      std::pow(std::max(0.0f, xyz.second), inverseN),
      std::pow(std::max(0.0f, xyz.third), inverseN));
}

FloatColor pseudoReflectanceToLab(const FloatColor& value) {
  return xyzToLab(FloatColor(
      std::pow(std::max(0.0f, value.first), kYuleNielsenN),
      std::pow(std::max(0.0f, value.second), kYuleNielsenN),
      std::pow(std::max(0.0f, value.third), kYuleNielsenN)));
}

const std::vector<FloatColor>& measuredPseudoReflectance() {
  static std::vector<FloatColor> palette;
  if (palette.empty()) {
    palette.reserve(6);
    for (size_t index = 0; index < 6; ++index) {
      palette.push_back(labToPseudoReflectance(kMeasuredPaperColorLab[index]));
    }
  }
  return palette;
}

float perceptualDistanceSquared(const FloatColor& target, size_t paletteIndex) {
  const FloatColor& candidate = kMeasuredPaperColorLab[paletteIndex];
  const float deltaLightness = target.first - candidate.first;
  const float deltaA = target.second - candidate.second;
  const float deltaB = target.third - candidate.third;
  return deltaLightness * deltaLightness * 1.15f + deltaA * deltaA + deltaB * deltaB;
}

size_t nearestMeasuredInk(const FloatColor& target, const RgbPixel* source) {
  size_t selected = 0;
  float best = perceptualDistanceSquared(target, 0);
  const int sourceMaximum = source
      ? std::max(source->red, std::max(source->green, source->blue)) : 255;
  const int sourceMinimum = source
      ? std::min(source->red, std::min(source->green, source->blue)) : 0;
  const float neutral = source
      ? clampFloat((42.0f - static_cast<float>(sourceMaximum - sourceMinimum)) / 42.0f,
                   0.0f, 1.0f)
      : 0.0f;
  if (neutral > 0.0f) best += 0.0f;
  for (size_t index = 1; index < 6; ++index) {
    float distance = perceptualDistanceSquared(target, index);
    // Suppress isolated chromatic confetti in neutral photos. Yellow remains
    // exempt because it is close to the panel paper lightness.
    if (neutral > 0.0f && (index == 3 || index == 4 || index == 5)) {
      distance += 72.0f * neutral;
    }
    if (distance < best) {
      selected = index;
      best = distance;
    }
  }
  return selected;
}

void addReflectanceError(
    std::vector<FloatColor>* row,
    int x,
    const FloatColor& error,
    float weight) {
  if (!row || x < 0 || x >= static_cast<int>(kPaperColorWidth)) return;
  FloatColor& target = (*row)[static_cast<size_t>(x)];
  target.first = clampFloat(target.first + error.first * weight, -0.6f, 0.6f);
  target.second = clampFloat(target.second + error.second * weight, -0.6f, 0.6f);
  target.third = clampFloat(target.third + error.third * weight, -0.6f, 0.6f);
}

bool renderReflectancePhoto(
    IPixelSource& source,
    IPixelSink& sink,
    std::string* error,
    IRenderProgress* progress) {
  std::vector<FloatColor> current(kPaperColorWidth);
  std::vector<FloatColor> next(kPaperColorWidth);
  std::vector<FloatColor> afterNext(kPaperColorWidth);
  const std::vector<FloatColor>& paletteP = measuredPseudoReflectance();
  const size_t pixelCount = static_cast<size_t>(kPaperColorWidth) * kPaperColorHeight;
  for (uint16_t y = 0; y < kPaperColorHeight; ++y) {
    const bool reverse = (y & 1U) != 0U;
    const int start = reverse ? static_cast<int>(kPaperColorWidth) - 1 : 0;
    const int end = reverse ? -1 : static_cast<int>(kPaperColorWidth);
    const int direction = reverse ? -1 : 1;
    // Pixel sources are forward-only. Buffer one RGB row so the quantizer can
    // use serpentine traversal without materializing the entire image.
    std::vector<RgbPixel> row(kPaperColorWidth);
    for (uint16_t x = 0; x < kPaperColorWidth; ++x) {
      if (!source.read(&row[x])) {
        if (error) *error = "pixel_source_ended_early";
        return false;
      }
    }
    std::vector<RgbPixel> outputRow(kPaperColorWidth);
    for (int x = start; x != end; x += direction) {
      const FloatColor targetLab = sourceToPanelLab(row[static_cast<size_t>(x)]);
      FloatColor targetP = labToPseudoReflectance(targetLab);
      targetP.first += current[static_cast<size_t>(x)].first;
      targetP.second += current[static_cast<size_t>(x)].second;
      targetP.third += current[static_cast<size_t>(x)].third;
      const FloatColor workingLab = pseudoReflectanceToLab(targetP);
      const size_t selected = nearestMeasuredInk(
          workingLab, &row[static_cast<size_t>(x)]);
      outputRow[static_cast<size_t>(x)] = paperColorPalette()[selected];
      FloatColor residual(
          targetP.first - paletteP[selected].first,
          targetP.second - paletteP[selected].second,
          targetP.third - paletteP[selected].third);
      const float residualDistance = std::sqrt(
          perceptualDistanceSquared(workingLab, selected));
      const float diffuse = residualDistance <= 4.0f
          ? 0.0f
          : residualDistance < 10.0f ? (residualDistance - 4.0f) / 6.0f : 1.0f;
      residual.first *= diffuse;
      residual.second *= diffuse;
      residual.third *= diffuse;
      addReflectanceError(&current, x + direction, residual, 8.0f / 42.0f);
      addReflectanceError(&current, x + direction * 2, residual, 4.0f / 42.0f);
      addReflectanceError(&next, x - direction * 2, residual, 2.0f / 42.0f);
      addReflectanceError(&next, x - direction, residual, 4.0f / 42.0f);
      addReflectanceError(&next, x, residual, 8.0f / 42.0f);
      addReflectanceError(&next, x + direction, residual, 4.0f / 42.0f);
      addReflectanceError(&next, x + direction * 2, residual, 2.0f / 42.0f);
      addReflectanceError(&afterNext, x - direction * 2, residual, 1.0f / 42.0f);
      addReflectanceError(&afterNext, x - direction, residual, 2.0f / 42.0f);
      addReflectanceError(&afterNext, x, residual, 4.0f / 42.0f);
      addReflectanceError(&afterNext, x + direction, residual, 2.0f / 42.0f);
      addReflectanceError(&afterNext, x + direction * 2, residual, 1.0f / 42.0f);
    }
    for (uint16_t x = 0; x < kPaperColorWidth; ++x) {
      if (!sink.write(outputRow[x])) {
        if (error) *error = "pixel_sink_rejected_data";
        return false;
      }
    }
    current.swap(next);
    next.swap(afterNext);
    std::fill(afterNext.begin(), afterNext.end(), FloatColor());
    if (progress) progress->onRenderProgress(
        static_cast<size_t>(y + 1U) * kPaperColorWidth, pixelCount);
  }
  return true;
}

bool renderSolidClean(
    IPixelSource& source,
    IPixelSink& sink,
    std::string* error,
    IRenderProgress* progress) {
  const size_t pixelCount = static_cast<size_t>(kPaperColorWidth) * kPaperColorHeight;
  for (size_t index = 0; index < pixelCount; ++index) {
    RgbPixel input;
    if (!source.read(&input)) {
      if (error) *error = "pixel_source_ended_early";
      return false;
    }
    const size_t selected = nearestMeasuredInk(sourceToPanelLab(input), &input);
    if (!sink.write(paperColorPalette()[selected])) {
      if (error) *error = "pixel_sink_rejected_data";
      return false;
    }
    if (progress && ((index + 1U) % kPaperColorWidth == 0U)) {
      progress->onRenderProgress(index + 1U, pixelCount);
    }
  }
  return true;
}

void addError(ErrorPixel* target, int32_t red, int32_t green, int32_t blue, int weight) {
  target->red += red * weight;
  target->green += green * weight;
  target->blue += blue * weight;
}

bool ensureNoExtraPixel(IPixelSource& source, std::string* error) {
  RgbPixel extra;
  if (source.read(&extra)) {
    if (error) *error = "pixel_source_has_extra_data";
    return false;
  }
  return true;
}

}  // namespace

ImageValidation validateImageMetadata(
    const ImageMetadata& metadata,
    uint64_t maximumEncodedBytes) {
  ImageValidation result;
  if (!validImageFormat(metadata.format)) {
    result.error = ImageValidationError::UnsupportedFormat;
    return result;
  }
  if (metadata.width == 0 || metadata.height == 0 ||
      metadata.width > kMaximumDimension || metadata.height > kMaximumDimension) {
    result.error = ImageValidationError::InvalidDimensions;
    return result;
  }
  if (metadata.encodedBytes == 0 || maximumEncodedBytes == 0 ||
      metadata.encodedBytes > maximumEncodedBytes) {
    result.error = ImageValidationError::EncodedSizeOutOfRange;
    return result;
  }
  const uint64_t pixels = static_cast<uint64_t>(metadata.width) * metadata.height;
  if (pixels > UINT64_MAX / 3ULL) {
    result.error = ImageValidationError::DecodedSizeOverflow;
    return result;
  }
  result.decodedRgbBytes = pixels * 3ULL;
  if (!metadata.orientationNormalized) {
    result.error = ImageValidationError::OrientationNotNormalized;
    return result;
  }
  result.valid = true;
  result.error = ImageValidationError::None;
  return result;
}

GeometryPlan planPaperColorGeometry(
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    FitMode mode) {
  GeometryPlan plan;
  if (!validFitMode(mode)) return plan;
  plan.mode = mode;
  if (sourceWidth == 0 || sourceHeight == 0 ||
      sourceWidth > kMaximumDimension || sourceHeight > kMaximumDimension) {
    return plan;
  }
  const uint64_t sourceScaledToTargetHeight =
      static_cast<uint64_t>(sourceWidth) * kPaperColorHeight;
  const uint64_t targetScaledToSourceHeight =
      static_cast<uint64_t>(kPaperColorWidth) * sourceHeight;

  if (mode == FitMode::CoverCrop) {
    plan.destination = RectU32(0, 0, kPaperColorWidth, kPaperColorHeight);
    if (sourceScaledToTargetHeight > targetScaledToSourceHeight) {
      const uint32_t cropWidth = static_cast<uint32_t>(
          static_cast<uint64_t>(sourceHeight) * kPaperColorWidth / kPaperColorHeight);
      plan.source = RectU32((sourceWidth - cropWidth) / 2U, 0, cropWidth, sourceHeight);
    } else if (sourceScaledToTargetHeight < targetScaledToSourceHeight) {
      const uint32_t cropHeight = static_cast<uint32_t>(
          static_cast<uint64_t>(sourceWidth) * kPaperColorHeight / kPaperColorWidth);
      plan.source = RectU32(0, (sourceHeight - cropHeight) / 2U, sourceWidth, cropHeight);
    } else {
      plan.source = RectU32(0, 0, sourceWidth, sourceHeight);
    }
  } else {
    plan.clearTargetToWhite = true;
    plan.source = RectU32(0, 0, sourceWidth, sourceHeight);
    if (sourceScaledToTargetHeight > targetScaledToSourceHeight) {
      const uint32_t destinationHeight = static_cast<uint32_t>(
          static_cast<uint64_t>(kPaperColorWidth) * sourceHeight / sourceWidth);
      plan.destination = RectU32(
          0, (kPaperColorHeight - destinationHeight) / 2U,
          kPaperColorWidth, destinationHeight);
    } else if (sourceScaledToTargetHeight < targetScaledToSourceHeight) {
      const uint32_t destinationWidth = static_cast<uint32_t>(
          static_cast<uint64_t>(kPaperColorHeight) * sourceWidth / sourceHeight);
      plan.destination = RectU32(
          (kPaperColorWidth - destinationWidth) / 2U, 0,
          destinationWidth, kPaperColorHeight);
    } else {
      plan.destination = RectU32(0, 0, kPaperColorWidth, kPaperColorHeight);
    }
  }
  plan.valid = plan.source.width > 0 && plan.source.height > 0 &&
      plan.destination.width > 0 && plan.destination.height > 0;
  return plan;
}

RenderPolicyDescriptor renderPolicy(RenderStrategy strategy) {
  if (strategy == RenderStrategy::ExperimentalSixColor) {
    return RenderPolicyDescriptor{
        true, "papercolor-sixcolor-rgb-fs-v1", true, true, true, false};
  }
  if (strategy == RenderStrategy::ReflectancePhoto) {
    return RenderPolicyDescriptor{
        true, "papercolor-reflectance-yn-stucki-v1", true, true, true, false};
  }
  if (strategy == RenderStrategy::SolidClean) {
    return RenderPolicyDescriptor{
        true, "papercolor-solid-clean-v1", false, true, true, false};
  }
  if (strategy == RenderStrategy::OfficialQuality) {
    return RenderPolicyDescriptor{
        true, "papercolor-m5gfx-quality-v1", false, false, true, false};
  }
  return RenderPolicyDescriptor{
      false, "invalid-render-strategy", false, false, false, false};
}

bool validImageFormat(ImageFormat format) {
  return format == ImageFormat::Png || format == ImageFormat::Jpeg ||
      format == ImageFormat::Bmp;
}

bool validFitMode(FitMode mode) {
  return mode == FitMode::CoverCrop || mode == FitMode::ContainLetterbox;
}

bool validRenderStrategy(RenderStrategy strategy) {
  return strategy == RenderStrategy::OfficialQuality ||
      strategy == RenderStrategy::ExperimentalSixColor ||
      strategy == RenderStrategy::ReflectancePhoto ||
      strategy == RenderStrategy::SolidClean;
}

const char* renderStrategyId(RenderStrategy strategy) {
  switch (strategy) {
    case RenderStrategy::OfficialQuality:
      return "official-quality";
    case RenderStrategy::ExperimentalSixColor:
      return "classic-six-color";
    case RenderStrategy::ReflectancePhoto:
      return "reflectance-photo";
    case RenderStrategy::SolidClean:
      return "solid-clean";
  }
  return "";
}

bool parseRenderStrategyId(const char* value, RenderStrategy* strategy) {
  if (!value || !strategy) return false;
  if (std::strcmp(value, "official-quality") == 0 ||
      std::strcmp(value, "official") == 0) {
    *strategy = RenderStrategy::OfficialQuality;
    return true;
  }
  if (std::strcmp(value, "classic-six-color") == 0 ||
      std::strcmp(value, "experimental-six-color") == 0) {
    *strategy = RenderStrategy::ExperimentalSixColor;
    return true;
  }
  if (std::strcmp(value, "reflectance-photo") == 0) {
    *strategy = RenderStrategy::ReflectancePhoto;
    return true;
  }
  if (std::strcmp(value, "solid-clean") == 0 ||
      std::strcmp(value, "inkloop-text") == 0) {
    *strategy = RenderStrategy::SolidClean;
    return true;
  }
  return false;
}

const std::vector<RgbPixel>& paperColorPalette() {
  static const std::vector<RgbPixel> palette = {
      RgbPixel(0, 0, 0),
      RgbPixel(255, 255, 255),
      RgbPixel(255, 243, 56),
      RgbPixel(191, 0, 0),
      RgbPixel(100, 64, 255),
      RgbPixel(67, 138, 28),
  };
  return palette;
}

bool isPaperColorPalettePixel(const RgbPixel& pixel) {
  const std::vector<RgbPixel>& palette = paperColorPalette();
  for (size_t index = 0; index < palette.size(); ++index) {
    if (samePixel(pixel, palette[index])) return true;
  }
  return false;
}

RgbPixel nearestPaperColorColor(const RgbPixel& pixel) {
  const std::vector<RgbPixel>& palette = paperColorPalette();
  size_t selected = 0;
  uint64_t selectedDistance = squaredDistance(pixel, palette[0]);
  for (size_t index = 1; index < palette.size(); ++index) {
    const uint64_t distance = squaredDistance(pixel, palette[index]);
    if (distance < selectedDistance) {
      selected = index;
      selectedDistance = distance;
    }
  }
  return palette[selected];
}

bool streamRenderPixels(
    IPixelSource& source,
    IPixelSink& sink,
    RenderStrategy strategy,
    std::string* error,
    IRenderProgress* progress) {
  if (!validRenderStrategy(strategy)) {
    if (error) *error = "invalid_render_strategy";
    return false;
  }
  if (source.width() != kPaperColorWidth || source.height() != kPaperColorHeight) {
    if (error) *error = "pixel_source_must_be_400x600";
    return false;
  }
  const size_t pixelCount = static_cast<size_t>(kPaperColorWidth) * kPaperColorHeight;
  if (strategy == RenderStrategy::OfficialQuality) {
    for (size_t index = 0; index < pixelCount; ++index) {
      RgbPixel pixel;
      if (!source.read(&pixel)) {
        if (error) *error = "pixel_source_ended_early";
        return false;
      }
      if (!sink.write(pixel)) {
        if (error) *error = "pixel_sink_rejected_data";
        return false;
      }
      if (progress && ((index + 1U) % kPaperColorWidth == 0U)) {
        progress->onRenderProgress(index + 1U, pixelCount);
      }
    }
  } else if (strategy == RenderStrategy::ExperimentalSixColor) {
    std::vector<ErrorPixel> current(kPaperColorWidth + 2U);
    std::vector<ErrorPixel> next(kPaperColorWidth + 2U);
    for (uint16_t y = 0; y < kPaperColorHeight; ++y) {
      for (uint16_t x = 0; x < kPaperColorWidth; ++x) {
        RgbPixel input;
        if (!source.read(&input)) {
          if (error) *error = "pixel_source_ended_early";
          return false;
        }
        const size_t errorIndex = static_cast<size_t>(x) + 1U;
        const RgbPixel adjusted(
            clampByte(static_cast<int32_t>(input.red) + roundedDivide16(current[errorIndex].red)),
            clampByte(static_cast<int32_t>(input.green) + roundedDivide16(current[errorIndex].green)),
            clampByte(static_cast<int32_t>(input.blue) + roundedDivide16(current[errorIndex].blue)));
        const RgbPixel output = nearestPaperColorColor(adjusted);
        if (!sink.write(output)) {
          if (error) *error = "pixel_sink_rejected_data";
          return false;
        }
        const int32_t redError = static_cast<int32_t>(adjusted.red) - output.red;
        const int32_t greenError = static_cast<int32_t>(adjusted.green) - output.green;
        const int32_t blueError = static_cast<int32_t>(adjusted.blue) - output.blue;
        addError(&current[errorIndex + 1U], redError, greenError, blueError, 7);
        addError(&next[errorIndex - 1U], redError, greenError, blueError, 3);
        addError(&next[errorIndex], redError, greenError, blueError, 5);
        addError(&next[errorIndex + 1U], redError, greenError, blueError, 1);
      }
      current.swap(next);
      std::fill(next.begin(), next.end(), ErrorPixel());
      if (progress) {
        progress->onRenderProgress(
            static_cast<size_t>(y + 1U) * kPaperColorWidth,
            pixelCount);
      }
    }
  } else if (strategy == RenderStrategy::ReflectancePhoto) {
    if (!renderReflectancePhoto(source, sink, error, progress)) return false;
  } else {
    if (!renderSolidClean(source, sink, error, progress)) return false;
  }
  if (!ensureNoExtraPixel(source, error)) return false;
  if (!sink.finish()) {
    if (error) *error = "pixel_sink_finish_failed";
    return false;
  }
  if (error) error->clear();
  return true;
}

}  // namespace displaypower
}  // namespace inkloop
