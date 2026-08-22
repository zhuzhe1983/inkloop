idf_component_register(
  SRCS
    "wifi_station_core.cpp"
    "wifi_provisioning_portal.cpp"
    "esp_wifi_provisioning_server.cpp"
    "esp_wifi_station.cpp"
  INCLUDE_DIRS "include"
  REQUIRES esp_event esp_http_server esp_netif esp_timer esp_wifi freertos lwip
)
