#pragma once

#include <cstdint>

#include "inkloop/board.hpp"

namespace inkloop {

struct MockBoardObservations {
  uint32_t initializations = 0;
  uint32_t shutdowns = 0;
  uint32_t display_accesses = 0;
  uint32_t renderer_accesses = 0;
  uint32_t rgb_frame_renders = 0;
  uint32_t frame_writes = 0;
  uint32_t audio_codec_accesses = 0;
  uint32_t button_gpio_reads = 0;
  uint32_t button_state_reads = 0;
  uint32_t rgb_writes = 0;
  uint32_t sd_preparations = 0;
};

MockBoardObservations mock_board_observations();
void mock_board_reset();
void mock_board_set_next_pressed(bool pressed);

}  // namespace inkloop
