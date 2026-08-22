set(inkloop_board_component "$ENV{INKLOOP_BOARD_COMPONENT}")
if(inkloop_board_component STREQUAL "")
  message(FATAL_ERROR "INKLOOP_BOARD_COMPONENT must name a board component")
endif()

message(STATUS "Inkloop board component: ${inkloop_board_component}")

idf_component_register(
  SRCS "app_main.cpp" "ota_outcome_journal.cpp" "ota_update_owner.cpp"
       "recovery_action_owner.cpp"
  REQUIRES
    inkloop_audio
    inkloop_audio_idf
    inkloop_board
    inkloop_contracts
    inkloop_connectivity
    inkloop_myai
    inkloop_ota
    inkloop_product
    inkloop_recovery
    inkloop_runtime
    inkloop_storage
    inkloop_voice
  PRIV_REQUIRES
    "${inkloop_board_component}"
)
