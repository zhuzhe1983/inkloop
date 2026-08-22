idf_component_register(
  SRCS "esp_portal_server.cpp"
  INCLUDE_DIRS "include"
  REQUIRES esp_http_server esp_timer freertos inkloop_portal lwip
)
