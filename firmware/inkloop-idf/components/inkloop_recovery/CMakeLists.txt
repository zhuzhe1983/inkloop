idf_component_register(
  SRCS
    "recovery_portal.cpp"
    "recovery_network_config.cpp"
    "esp_recovery_server.cpp"
    "recovery_network_owner.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    esp_http_server
    esp_hw_support
    esp_timer
    inkloop_connectivity
    lwip
    mdns
)
