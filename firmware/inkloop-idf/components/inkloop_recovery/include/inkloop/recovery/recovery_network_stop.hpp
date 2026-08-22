#pragma once

#include <memory>
#include <utility>

#include "esp_err.h"

namespace inkloop {
namespace recovery {
namespace detail {

// Shared by the production owner and the host fault matrix. The station
// object remains alive after either stop failure; only a real ESP_OK from both
// owners permits its destructor to run.
template <typename StationOwner, typename StopServices>
esp_err_t stopRecoveryNetworkOwners(
    std::unique_ptr<StationOwner>& station,
    StopServices&& stop_services) {
  const esp_err_t services = std::forward<StopServices>(stop_services)();
  if (services != ESP_OK) return services;
  if (!station) return ESP_OK;

  const esp_err_t station_status = station->shutdown();
  if (station_status != ESP_OK) return station_status;
  station.reset();
  return ESP_OK;
}

}  // namespace detail
}  // namespace recovery
}  // namespace inkloop
