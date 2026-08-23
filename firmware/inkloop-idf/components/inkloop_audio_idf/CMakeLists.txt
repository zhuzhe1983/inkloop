idf_component_register(
  SRCS "esp_i2s_audio.cpp" "esp_cross_core_audio_bridge.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    esp_driver_gpio
    esp_driver_i2s
    esp_timer
    esp_psram
    freertos
    heap
    inkloop_audio
    inkloop_myai
)
