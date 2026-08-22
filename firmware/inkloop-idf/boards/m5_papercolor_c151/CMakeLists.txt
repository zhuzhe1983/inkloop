idf_component_register(
  SRCS
    "board.cpp"
    "papercolor_audio_codec.cpp"
    "papercolor_ed2208.cpp"
    "papercolor_ed2208_protocol.cpp"
    "papercolor_renderer.cpp"
  INCLUDE_DIRS "include"
  REQUIRES inkloop_audio_idf inkloop_board inkloop_onboarding
    inkloop_onboarding_idf inkloop_render m5stack__m5pm1 espressif__led_strip
  PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_spi freertos
)
