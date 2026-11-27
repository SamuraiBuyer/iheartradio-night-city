// iHeart Radio - builds the station logo .archive (C++ port of src/manager/IconArchive.cs, same output).
//
// Each station logo becomes two game files, made from templates built once with WolvenKit (src/manager/templates,
// built into the DLL as resources):
//   iheartradio\icons\iheart_<8-digit id>.xbm       512x512 BC7 texture (CBitmapTexture, like the game's radio icons)
//   iheartradio\icons\iheart_<8-digit id>.inkatlas  atlas with one full-size part named iheart_<8-digit id>
// Stored uncompressed (segment size == stored size), which the game reads as is.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace IconArchive
{
constexpr int kIconSize = 512;

std::string IconName(long long aStationId);    // iheart_00001469
std::string AtlasPath(long long aStationId);   // iheartradio\icons\iheart_00001469.inkatlas

// aIcons: station id + 512x512 alpha (white icon), top row first. False (with aError) when it can't be built.
bool Build(const std::vector<std::pair<long long, std::vector<uint8_t>>>& aIcons, std::string& aOut, std::string& aError);
} // namespace IconArchive
