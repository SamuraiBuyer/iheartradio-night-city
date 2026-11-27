// iHeart Radio - local stream bridge, running inside the game process (started by the RED4ext plugin).
//
// RadioExt plays web streams with FMOD, which can't decode iHeart's AAC / HLS audio. The bridge turns any iHeart
// station into a stream FMOD can play, on this PC only (127.0.0.1, nothing is reachable from other computers):
//     http://127.0.0.1:8925/zc<station id>.ogg       e.g. http://127.0.0.1:8925/zc1469.ogg  (Z100)
// The stream is decoded inside the game (AAC: Windows' decoder, or the built-in fdk-aac under CrossOver / Wine and
// where Windows has none; MP3: minimp3) and sent as Ogg Vorbis. FMOD picks the format by the URL's extension, so
// the URL must end in .ogg. Older /zc<id>.mp3 URLs still work (WAV, about 3 hours at a time) and are switched to
// .ogg when the plugin starts.
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
// (port already in use, ...).
bool Start(const std::filesystem::path& aPluginDir, std::string& aError);
void Stop();
} // namespace Bridge
