#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace inkloop {
namespace displaypower {

static const uint16_t kPaperColorWidth = 400;
static const uint16_t kPaperColorHeight = 600;

enum class ImageFormat : uint8_t { Png, Jpeg, Bmp, Unknown };
enum class FitMode : uint8_t { CoverCrop, ContainLetterbox };
enum class RenderStrategy : uint8_t {
  OfficialQuality = 0,
  ExperimentalSixColor = 1,
  ReflectancePhoto = 2,
  SolidClean = 3,
};

struct ImageMetadata {
  ImageFormat format;
  uint32_t width;
  uint32_t height;
  uint64_t encodedBytes;
  bool orientationNormalized;

  ImageMetadata()
      : format(ImageFormat::Unknown),
        width(0),
        height(0),
        encodedBytes(0),
        orientationNormalized(false) {}
};

enum class ImageValidationError : uint8_t {
  None,
  UnsupportedFormat,
  InvalidDimensions,
  EncodedSizeOutOfRange,
  DecodedSizeOverflow,
  OrientationNotNormalized,
};

struct ImageValidation {
  bool valid;
  ImageValidationError error;
  uint64_t decodedRgbBytes;

  ImageValidation()
      : valid(false), error(ImageValidationError::InvalidDimensions), decodedRgbBytes(0) {}
};

struct RectU32 {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;

  RectU32() : x(0), y(0), width(0), height(0) {}
  RectU32(uint32_t xValue, uint32_t yValue, uint32_t widthValue, uint32_t heightValue)
      : x(xValue), y(yValue), width(widthValue), height(heightValue) {}
};

struct GeometryPlan {
  bool valid;
  FitMode mode;
  RectU32 source;
  RectU32 destination;
  uint16_t targetWidth;
  uint16_t targetHeight;
  bool clearTargetToWhite;

  GeometryPlan()
      : valid(false),
        mode(FitMode::CoverCrop),
        source(),
        destination(),
        targetWidth(kPaperColorWidth),
        targetHeight(kPaperColorHeight),
        clearTargetToWhite(false) {}
};

struct RgbPixel {
  uint8_t red;
  uint8_t green;
  uint8_t blue;

  RgbPixel() : red(0), green(0), blue(0) {}
  RgbPixel(uint8_t redValue, uint8_t greenValue, uint8_t blueValue)
      : red(redValue), green(greenValue), blue(blueValue) {}
};

struct RenderPolicyDescriptor {
  bool valid;
  const char* id;
  bool experimental;
  bool preprocessesToSixColors;
  bool fullScreenRefreshRequired;
  bool partialRefreshSupported;
};

class IPixelSource {
 public:
  virtual ~IPixelSource() {}
  virtual uint16_t width() const = 0;
  virtual uint16_t height() const = 0;
  virtual bool read(RgbPixel* pixel) = 0;
};

class IPixelSink {
 public:
  virtual ~IPixelSink() {}
  virtual bool write(const RgbPixel& pixel) = 0;
  virtual bool finish() = 0;
};

class IRenderProgress {
 public:
  virtual ~IRenderProgress() {}
  virtual void onRenderProgress(size_t completedPixels, size_t totalPixels) = 0;
};

ImageValidation validateImageMetadata(
    const ImageMetadata& metadata,
    uint64_t maximumEncodedBytes = 16ULL * 1024ULL * 1024ULL);
GeometryPlan planPaperColorGeometry(
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    FitMode mode);
RenderPolicyDescriptor renderPolicy(RenderStrategy strategy);
bool validImageFormat(ImageFormat format);
bool validFitMode(FitMode mode);
bool validRenderStrategy(RenderStrategy strategy);
const char* renderStrategyId(RenderStrategy strategy);
bool parseRenderStrategyId(const char* value, RenderStrategy* strategy);
const std::vector<RgbPixel>& paperColorPalette();
bool isPaperColorPalettePixel(const RgbPixel& pixel);
RgbPixel nearestPaperColorColor(const RgbPixel& pixel);

// The source must already be normalized to exact 400x600 scanline RGB. The
// official policy reproduces M5GFX ED2208 epd_quality RGB-pair dithering. The
// classic policy performs deterministic RGB Floyd-Steinberg error diffusion;
// reflectance-photo uses measured PaperColor Lab anchors and Yule-Nielsen
// pseudo-reflectance Stucki diffusion; solid-clean disables diffusion for
// crisp text, tables, and large flat fills. Every policy requires a full-screen
// ED2208 refresh.
bool streamRenderPixels(
    IPixelSource& source,
    IPixelSink& sink,
    RenderStrategy strategy,
    std::string* error,
    IRenderProgress* progress = 0);

}  // namespace displaypower
}  // namespace inkloop
