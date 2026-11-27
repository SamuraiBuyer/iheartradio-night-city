// iHeart Radio - local MP3 bridge, running inside the game process (started by the RED4ext plugin).
//
// RadioExt plays web streams with FMOD, which can't decode iHeart's AAC / HLS audio. The bridge turns any iHeart
// station into a plain MP3 stream on this PC only (127.0.0.1, nothing is reachable from other computers):
//     http://127.0.0.1:8925/zc<station id>.mp3       e.g. http://127.0.0.1:8925/zc1469.mp3  (Z100)
// Each connection runs one ffmpeg (AAC -> MP3). The ffmpeg processes sit in a job object that Windows closes with
// the game, so they never outlive it.
//
// Now playing: bin\x64\plugins\cyber_engine_tweaks\mods\iHeartRadio\data\nowplaying.txt for the CET part, one line
// per station the game is listening to: <station id> TAB <artist> TAB <title> (empty during ads / DJ talk).
// Log: red4ext\plugins\iHeartRadio\bridge.log
#pragma once

#include <filesystem>
#include <string>

namespace Bridge
{
// Starts the server and the now-playing poller on background threads. Returns false and fills aError when it can't
// (ffmpeg missing, port already in use, ...).
bool Start(const std::filesystem::path& aPluginDir, std::string& aError);
void Stop();
} // namespace Bridge
