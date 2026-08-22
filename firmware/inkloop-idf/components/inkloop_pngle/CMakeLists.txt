idf_component_register(
  SRCS "pngle.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_rom
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE PNGLE_NO_GAMMA_CORRECTION=1)
