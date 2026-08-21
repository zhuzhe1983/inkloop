#include "ImageProcessing.h"

#include <limits.h>

#include <algorithm>

namespace inkloop {
namespace displaypower {

namespace {

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
        true, "papercolor-sixcolor-prequant-v1", true, true, true, false};
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
      strategy == RenderStrategy::ExperimentalSixColor;
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
  } else {
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
