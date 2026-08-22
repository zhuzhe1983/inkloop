#include "inkloop/esp_ota_boot_health.hpp"

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace inkloop {
namespace {

EspOtaStateReadCode readRunningImageState(OtaRunningImageState& output) {
  output = OtaRunningImageState::Unknown;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return EspOtaStateReadCode::RunningPartitionUnavailable;

  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t status = esp_ota_get_state_partition(running, &state);
  if (status == ESP_ERR_NOT_SUPPORTED &&
      running->type == ESP_PARTITION_TYPE_APP &&
      running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
    output = OtaRunningImageState::Ordinary;
    return EspOtaStateReadCode::Ok;
  }
  if (status != ESP_OK) return EspOtaStateReadCode::StateUnavailable;
  switch (state) {
    case ESP_OTA_IMG_PENDING_VERIFY:
      output = OtaRunningImageState::PendingVerify;
      return EspOtaStateReadCode::Ok;
    case ESP_OTA_IMG_VALID:
      output = OtaRunningImageState::Confirmed;
      return EspOtaStateReadCode::Ok;
    case ESP_OTA_IMG_UNDEFINED:
      output = OtaRunningImageState::Ordinary;
      return EspOtaStateReadCode::Ok;
    case ESP_OTA_IMG_INVALID:
    case ESP_OTA_IMG_ABORTED:
      output = OtaRunningImageState::Invalid;
      return EspOtaStateReadCode::Ok;
    case ESP_OTA_IMG_NEW:
      output = OtaRunningImageState::Unknown;
      return EspOtaStateReadCode::Ok;
  }
  return EspOtaStateReadCode::StateUnavailable;
}

int markAppValidCancelRollback() {
  return static_cast<int>(esp_ota_mark_app_valid_cancel_rollback());
}

int markAppInvalidRollbackAndReboot() {
  return static_cast<int>(esp_ota_mark_app_invalid_rollback_and_reboot());
}

constexpr EspOtaSystemFunctions kSystemFunctions{
    &readRunningImageState,
    &markAppValidCancelRollback,
    &markAppInvalidRollbackAndReboot,
};

}  // namespace

const EspOtaSystemFunctions& systemEspOtaFunctions() {
  return kSystemFunctions;
}

}  // namespace inkloop
