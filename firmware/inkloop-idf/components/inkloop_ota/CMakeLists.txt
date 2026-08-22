idf_component_register(
  SRCS
    "esp_ota_boot_health.cpp"
    "esp_ota_ed25519_verifier.cpp"
    "esp_ota_https_transport.cpp"
    "esp_ota_staging.cpp"
    "esp_ota_staging_system_api.cpp"
    "esp_ota_system_api.cpp"
    "ota_boot_health.cpp"
    "ota_runtime_health.cpp"
    "ota_sha256.cpp"
    "ota_https_acquisition.cpp"
    "ota_staging.cpp"
  INCLUDE_DIRS "include"
  REQUIRES app_update inkloop_runtime libsodium
  PRIV_REQUIRES esp_http_client esp-tls esp_timer lwip
)
