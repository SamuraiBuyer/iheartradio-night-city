// iHeart Radio - the station manager inside the game (what the CET menu calls, through Script.cpp).
//
// Writes the same files as the Station Manager app, so both can be used:
//   radioExt\radios\iheart_<id>_<name>\metadata.json        the RadioExt station
//   mods\iHeartRadio\data\logos\<id>.png + <id>.json        the logo (512x512 white on transparent) + its style
//   archive\pc\mod\iHeartRadio_Stations.archive             the logos as game textures
// The game keeps the archive open while it runs: then the new one is saved as
// mods\iHeartRadio\data\iHeartRadio_Stations.archive.next and put in place when the game starts next (this plugin
// loads before the game reads its archives).
//
// Requests and answers are JSON:
//   Call:   {"op":"status"} {"op":"list"}                                                   (quick, answered at once)
//           {"op":"preview","id":"1469","key":"solid"|"current","invert":false,"gap":3,"size":64}
//   Begin:  {"op":"search","q":"z100"} {"op":"thumb","url":"https://i.iheart.com/...","size":40}
//           {"op":"station","id":"1469","gap":3} {"op":"styles","gap":3}
//           {"op":"install",...} {"op":"remove","id":"1469"} {"op":"removeAll"}          (run in the background)
//   Poll(ticket): "" while running, then the answer once.
#pragma once

#include <filesystem>
#include <string>

namespace Stations
{
void Init(const std::filesystem::path& aGameDir);   // at plugin load: puts a waiting logo archive in place
std::string Call(const std::string& aRequest);
int Begin(const std::string& aRequest);
std::string Poll(int aTicket);
} // namespace Stations
