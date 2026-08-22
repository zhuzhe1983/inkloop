idf_component_register(
  SRCS
    "esp_deep_sleep_adapter.cpp"
    "esp_sleep_system_api.cpp"
    "esp_board_wake_capabilities.cpp"
  INCLUDE_DIRS "include"
  REQUIRES inkloop_board inkloop_power esp_hw_support
)

