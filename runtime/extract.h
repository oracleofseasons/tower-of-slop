// Builds the game's pak from the original Game Maker 7 exe (see extract.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gml {

bool extract_pak(const std::string& exe_path, std::vector<uint8_t>& pak, std::string& error);

// The exe's own icon (its largest RT_ICON image, 32-bit or palette DIB) as RGBA, top row first.
bool extract_icon(const std::string& exe_path, int& w, int& h, std::vector<uint8_t>& rgba, std::string& error);

}  // namespace gml
