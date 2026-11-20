// Builds the game's pak from the original Game Maker 7 exe (see extract.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gml {

bool extract_pak(const std::string& exe_path, std::vector<uint8_t>& pak, std::string& error);

}  // namespace gml
