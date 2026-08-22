idf_component_register(
  SRCS "esp_nvs_settings_store.cpp"
  INCLUDE_DIRS "include"
  REQUIRES inkloop_settings
  PRIV_REQUIRES mbedtls nvs_flash
)

