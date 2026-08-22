#pragma once

#include <cstdint>
#include <string>

#include "inkloop/board.hpp"

namespace inkloop {

// Shared product wording and image geometry derived from the selected board.
// Keeping this policy here prevents Voice, Portal and MyAI identity code from
// silently drifting back to PaperColor-specific dimensions or labels.
std::string boardResolution(const BoardDescriptor& board);
std::string boardPanelSummary(const BoardDescriptor& board);
std::string defaultAssistantPrompt(const BoardDescriptor& board);
std::string defaultImagePromptTemplate(const BoardDescriptor& board);
std::string defaultNegativePrompt(const BoardDescriptor& board);
std::string myAiDeviceLabel(const BoardDescriptor& board);
std::string myAiInstallationFingerprintPrefix(const BoardDescriptor& board);
std::string aigcImageSize(const BoardDescriptor& board);

// Accept the board's canonical logical orientation and its exact 90-degree
// rotation. landscape is true only for the rotated form.
bool classifyBoardPngGeometry(const BoardDescriptor& board, uint32_t width,
                              uint32_t height, bool& landscape);

}  // namespace inkloop
