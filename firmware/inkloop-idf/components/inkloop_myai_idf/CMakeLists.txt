idf_component_register(
  SRCS
    "esp_aigc_output_transport.cpp"
    "esp_http_adapters.cpp"
    "esp_nvs_credential_store.cpp"
    "esp_wss_transport.cpp"
  INCLUDE_DIRS "include"
  REQUIRES inkloop_myai
  PRIV_REQUIRES esp_http_client esp-tls esp_timer freertos lwip mbedtls nvs_flash tcp_transport
)
