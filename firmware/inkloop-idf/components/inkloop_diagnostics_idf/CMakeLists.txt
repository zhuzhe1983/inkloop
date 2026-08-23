idf_component_register(
  SRCS "esp_serial_diagnostics.cpp"
  INCLUDE_DIRS "include"
  REQUIRES inkloop_diagnostics esp_common esp_driver_usb_serial_jtag freertos
)
