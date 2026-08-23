idf_component_register(
  SRCS "esp_serial_diagnostics.cpp"
  INCLUDE_DIRS "include"
  REQUIRES inkloop_diagnostics esp_common freertos
)
