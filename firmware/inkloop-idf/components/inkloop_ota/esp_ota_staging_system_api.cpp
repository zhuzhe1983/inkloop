#include "inkloop/esp_ota_staging.hpp"

#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace inkloop {
namespace {

EspOtaPartition getRunningPartition() {
  return esp_ota_get_running_partition();
}

EspOtaPartition getNextUpdatePartition() {
  return esp_ota_get_next_update_partition(nullptr);
}

std::uint64_t partitionCapacity(EspOtaPartition opaque_partition) {
  const auto* partition =
      static_cast<const esp_partition_t*>(opaque_partition);
  return partition ? static_cast<std::uint64_t>(partition->size) : 0U;
}

int otaBegin(EspOtaPartition opaque_partition, std::size_t image_size,
             EspOtaHandle& output_handle) {
  const auto* partition =
      static_cast<const esp_partition_t*>(opaque_partition);
  esp_ota_handle_t handle = 0;
  const esp_err_t status = esp_ota_begin(partition, image_size, &handle);
  if (status == ESP_OK) output_handle = static_cast<EspOtaHandle>(handle);
  return static_cast<int>(status);
}

int otaWrite(EspOtaHandle opaque_handle, const std::uint8_t* bytes,
             std::size_t length) {
  return static_cast<int>(esp_ota_write(
      static_cast<esp_ota_handle_t>(opaque_handle), bytes, length));
}

int otaEnd(EspOtaHandle opaque_handle) {
  return static_cast<int>(
      esp_ota_end(static_cast<esp_ota_handle_t>(opaque_handle)));
}

int otaAbort(EspOtaHandle opaque_handle) {
  return static_cast<int>(
      esp_ota_abort(static_cast<esp_ota_handle_t>(opaque_handle)));
}

int setBootPartition(EspOtaPartition opaque_partition) {
  const auto* partition =
      static_cast<const esp_partition_t*>(opaque_partition);
  return static_cast<int>(esp_ota_set_boot_partition(partition));
}

constexpr EspOtaWriterFunctions kSystemWriterFunctions{
    &getRunningPartition,
    &getNextUpdatePartition,
    &partitionCapacity,
    &otaBegin,
    &otaWrite,
    &otaEnd,
    &otaAbort,
    &setBootPartition,
};

}  // namespace

const EspOtaWriterFunctions& systemEspOtaWriterFunctions() {
  return kSystemWriterFunctions;
}

}  // namespace inkloop
