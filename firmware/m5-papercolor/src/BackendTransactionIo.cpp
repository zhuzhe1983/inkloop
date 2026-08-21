#include "BackendTransactionIo.h"

namespace inkloop {

bool BackendTransactionIo::writeAll(const char* path, const uint8_t* bytes, size_t length) {
  if (!available()) return false;
  File file = storage_.open(path, FILE_WRITE);
  if (!file) return false;
  const size_t written = file.write(bytes, length);
  file.flush();
  file.close();
  return written == length && available();
}

bool BackendTransactionIo::contentEquals(const char* path, const uint8_t* bytes, size_t length) {
  if (!available()) return false;
  File file = storage_.open(path, FILE_READ);
  if (!file || file.size() != length) {
    if (file) file.close();
    return false;
  }
  uint8_t buffer[512];
  size_t offset = 0;
  bool equal = true;
  while (offset < length) {
    const size_t chunk = length - offset < sizeof(buffer) ? length - offset : sizeof(buffer);
    const size_t received = file.read(buffer, chunk);
    if (received != chunk || memcmp(buffer, bytes + offset, chunk) != 0) {
      equal = false;
      break;
    }
    offset += received;
  }
  file.close();
  return equal && available();
}

}  // namespace inkloop
