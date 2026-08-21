#include "PaperColorStatusPng.h"

#include <esp_heap_caps.h>
#include <lgfx/utility/lgfx_qrcode.h>

#include <algorithm>
#include <cstring>

namespace inkloop {
namespace {

constexpr uint32_t kWidth = 400;
constexpr uint32_t kHeight = 600;
constexpr size_t kStride = 1U + kWidth * 3U;
constexpr size_t kRawBytes = kStride * kHeight;
constexpr uint8_t kQrVersion = 8;

const uint8_t kDigits[10][7] = {
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
    {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
    {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e},
    {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e},
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e},
};

void put32(uint8_t*& cursor, uint32_t value) {
  *cursor++ = static_cast<uint8_t>(value >> 24);
  *cursor++ = static_cast<uint8_t>(value >> 16);
  *cursor++ = static_cast<uint8_t>(value >> 8);
  *cursor++ = static_cast<uint8_t>(value);
}

uint32_t crc32Update(uint32_t crc, const uint8_t* bytes, size_t length) {
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc;
}

void writeChunk(
    uint8_t*& cursor, const char type[4], const uint8_t* data, size_t length) {
  put32(cursor, static_cast<uint32_t>(length));
  uint8_t* typeAt = cursor;
  memcpy(cursor, type, 4);
  cursor += 4;
  if (length) {
    memcpy(cursor, data, length);
    cursor += length;
  }
  uint32_t crc = crc32Update(0xffffffffU, typeAt, 4 + length) ^ 0xffffffffU;
  put32(cursor, crc);
}

void setPixel(uint8_t* raw, int x, int y, uint8_t red, uint8_t green,
              uint8_t blue) {
  if (!raw || x < 0 || y < 0 || x >= static_cast<int>(kWidth) ||
      y >= static_cast<int>(kHeight)) return;
  uint8_t* pixel = raw + static_cast<size_t>(y) * kStride + 1U +
      static_cast<size_t>(x) * 3U;
  pixel[0] = red;
  pixel[1] = green;
  pixel[2] = blue;
}

void fillRect(uint8_t* raw, int x, int y, int width, int height,
              uint8_t red, uint8_t green, uint8_t blue) {
  for (int row = 0; row < height; ++row)
    for (int column = 0; column < width; ++column)
      setPixel(raw, x + column, y + row, red, green, blue);
}

void drawDigit(uint8_t* raw, char digit, int x, int y, int scale) {
  if (digit < '0' || digit > '9') return;
  const uint8_t* rows = kDigits[digit - '0'];
  for (int row = 0; row < 7; ++row) {
    for (int column = 0; column < 5; ++column) {
      if (rows[row] & (1U << (4 - column)))
        fillRect(raw, x + column * scale, y + row * scale,
                 scale, scale, 20, 24, 22);
    }
  }
}

bool validHttpsBindingUrl(const std::string& bindingUrl) {
  if (bindingUrl.size() < 12 || bindingUrl.size() > 512 ||
      bindingUrl.compare(0, 8, "https://") != 0) return false;
  const size_t hostEnd = bindingUrl.find_first_of("/?#", 8);
  const size_t end = hostEnd == std::string::npos ? bindingUrl.size() : hostEnd;
  if (end <= 8 || bindingUrl.find('@', 8) < end) return false;
  for (size_t index = 8; index < bindingUrl.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(bindingUrl[index]);
    if (ch <= 0x20 || ch >= 0x7f || ch == '\\' || ch == '"') return false;
  }
  return true;
}

const uint8_t* glyph(char character) {
  static const uint8_t letters[26][7] = {
      {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
      {14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
      {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
      {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
      {14,4,4,4,4,4,14},{7,2,2,2,18,18,12},
      {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
      {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},
      {14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
      {14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
      {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
      {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},
      {17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
      {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}};
  return character >= 'A' && character <= 'Z'
      ? letters[character - 'A'] : nullptr;
}

void drawText(uint8_t* raw, const char* text, int y, int scale) {
  if (!raw || !text) return;
  const size_t length = strlen(text);
  const int advance = 6 * scale;
  int x = (static_cast<int>(kWidth) - static_cast<int>(length) * advance) / 2;
  for (size_t index = 0; index < length; ++index, x += advance) {
    const uint8_t* rows = glyph(text[index]);
    if (!rows) continue;
    for (int row = 0; row < 7; ++row)
      for (int column = 0; column < 5; ++column)
        if (rows[row] & (1U << (4 - column)))
          fillRect(raw, x + column * scale, y + row * scale,
                   scale, scale, 20, 24, 22);
  }
}

bool validPairingInputs(
    const std::string& code, const std::string& bindingUrl) {
  if (code.size() != 6 || bindingUrl.empty() || bindingUrl.size() > 512) return false;
  for (char character : code)
    if (character < '0' || character > '9') return false;
  return validHttpsBindingUrl(bindingUrl);
}

uint8_t* allocateWhiteCanvas() {
  uint8_t* raw = static_cast<uint8_t*>(
      heap_caps_malloc(kRawBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!raw) return nullptr;
  for (uint32_t y = 0; y < kHeight; ++y) {
    uint8_t* row = raw + static_cast<size_t>(y) * kStride;
    row[0] = 0;
    memset(row + 1, 0xff, kStride - 1);
  }
  return raw;
}

bool encodeRawPng(uint8_t* raw, GeneratedStatusPng& output) {
  if (!raw) return false;
  const size_t deflateBlocks = (kRawBytes + 65534U) / 65535U;
  const size_t zlibBytes = 2U + kRawBytes + deflateBlocks * 5U + 4U;
  const size_t pngBytes = 8U + (12U + 13U) + (12U + zlibBytes) + 12U;
  uint8_t* png = static_cast<uint8_t*>(
      heap_caps_malloc(pngBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t* compressed = static_cast<uint8_t*>(
      heap_caps_malloc(zlibBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!png || !compressed) {
    free(png);
    free(compressed);
    free(raw);
    return false;
  }
  uint8_t* z = compressed;
  *z++ = 0x78;
  *z++ = 0x01;
  size_t offset = 0;
  while (offset < kRawBytes) {
    const uint16_t block = static_cast<uint16_t>(
        std::min<size_t>(65535U, kRawBytes - offset));
    *z++ = offset + block == kRawBytes ? 0x01 : 0x00;
    *z++ = static_cast<uint8_t>(block);
    *z++ = static_cast<uint8_t>(block >> 8);
    const uint16_t inverse = static_cast<uint16_t>(~block);
    *z++ = static_cast<uint8_t>(inverse);
    *z++ = static_cast<uint8_t>(inverse >> 8);
    memcpy(z, raw + offset, block);
    z += block;
    offset += block;
  }
  uint32_t s1 = 1;
  uint32_t s2 = 0;
  for (size_t index = 0; index < kRawBytes; ++index) {
    s1 = (s1 + raw[index]) % 65521U;
    s2 = (s2 + s1) % 65521U;
  }
  *z++ = static_cast<uint8_t>(s2 >> 8);
  *z++ = static_cast<uint8_t>(s2);
  *z++ = static_cast<uint8_t>(s1 >> 8);
  *z++ = static_cast<uint8_t>(s1);
  free(raw);
  if (static_cast<size_t>(z - compressed) != zlibBytes) {
    free(png);
    free(compressed);
    return false;
  }

  uint8_t* cursor = png;
  const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  memcpy(cursor, signature, sizeof(signature));
  cursor += sizeof(signature);
  uint8_t ihdr[13] = {};
  uint8_t* ihdrCursor = ihdr;
  put32(ihdrCursor, kWidth);
  put32(ihdrCursor, kHeight);
  ihdr[8] = 8;
  ihdr[9] = 2;
  writeChunk(cursor, "IHDR", ihdr, sizeof(ihdr));
  writeChunk(cursor, "IDAT", compressed, zlibBytes);
  writeChunk(cursor, "IEND", nullptr, 0);
  free(compressed);
  if (static_cast<size_t>(cursor - png) != pngBytes) {
    free(png);
    return false;
  }
  output.bytes = png;
  output.length = pngBytes;
  return true;
}

}  // namespace

bool validPairingStatusInputs(
    const std::string& sixDigitCode, const std::string& bindingUrl) {
  return validPairingInputs(sixDigitCode, bindingUrl);
}

void GeneratedStatusPng::release() {
  free(bytes);
  bytes = nullptr;
  length = 0;
}

bool makePairingStatusPng(
    const std::string& sixDigitCode, const std::string& bindingUrl,
    GeneratedStatusPng& output) {
  output.release();
  if (!validPairingInputs(sixDigitCode, bindingUrl)) return false;
  uint8_t* raw = allocateWhiteCanvas();
  if (!raw) return false;

  const uint16_t qrBufferBytes = lgfx_qrcode_getBufferSize(kQrVersion);
  uint8_t* qrBuffer = static_cast<uint8_t*>(malloc(qrBufferBytes));
  QRCode qr = {};
  if (!qrBuffer || lgfx_qrcode_initText(
          &qr, qrBuffer, kQrVersion, ECC_MEDIUM, bindingUrl.c_str()) != 0) {
    free(qrBuffer);
    free(raw);
    return false;
  }
  const int moduleScale = 4;
  const int quietModules = 4;
  const int qrPixels = (qr.size + quietModules * 2) * moduleScale;
  const int qrX = (static_cast<int>(kWidth) - qrPixels) / 2;
  const int qrY = 140;
  fillRect(raw, qrX, qrY, qrPixels, qrPixels, 255, 255, 255);
  for (uint8_t y = 0; y < qr.size; ++y) {
    for (uint8_t x = 0; x < qr.size; ++x) {
      if (lgfx_qrcode_getModule(&qr, x, y)) {
        fillRect(raw,
                 qrX + (x + quietModules) * moduleScale,
                 qrY + (y + quietModules) * moduleScale,
                 moduleScale, moduleScale, 20, 24, 22);
      }
    }
  }
  free(qrBuffer);

  // Bottom-down 400x600 composition: QR above, six-digit code below, both
  // horizontally centered; the combined stack is vertically centered.
  constexpr int digitScale = 8;
  constexpr int digitAdvance = 48;
  constexpr int codeWidth = 5 * digitAdvance + 5 * digitScale;
  const int codeX = (static_cast<int>(kWidth) - codeWidth) / 2;
  const int codeY = 404;
  static_assert(codeWidth <= 320,
                "pairing code must stay inside the physical safe area");
  for (size_t index = 0; index < sixDigitCode.size(); ++index)
    drawDigit(raw, sixDigitCode[index], codeX + index * digitAdvance,
              codeY, digitScale);
  return encodeRawPng(raw, output);
}

bool makeBoundStatusPng(GeneratedStatusPng& output) {
  output.release();
  uint8_t* raw = allocateWhiteCanvas();
  if (!raw) return false;
  // A fixed, nonsecret ready mark: green check, yellow status bar, blue base.
  // No digit glyphs or QR encoder are invoked on this path.
  for (int offset = 0; offset < 18; ++offset) {
    fillRect(raw, 92 + offset, 282 + offset, 12, 70, 22, 142, 74);
    fillRect(raw, 110 + offset, 334 - offset, 12, 140, 22, 142, 74);
  }
  fillRect(raw, 40, 470, 320, 12, 247, 209, 0);
  fillRect(raw, 80, 510, 240, 8, 40, 100, 220);
  return encodeRawPng(raw, output);
}

bool makePairingUnavailableStatusPng(GeneratedStatusPng& output) {
  output.release();
  uint8_t* raw = allocateWhiteCanvas();
  if (!raw) return false;
  fillRect(raw, 0, 0, kWidth, 20, 220, 48, 36);
  drawText(raw, "SERVICE", 150, 6);
  drawText(raw, "UNAVAILABLE", 220, 5);
  drawText(raw, "CONTACT INKLOOP", 330, 3);
  drawText(raw, "DEVELOPER", 375, 4);
  fillRect(raw, 40, 470, 320, 12, 247, 209, 0);
  return encodeRawPng(raw, output);
}

}  // namespace inkloop
