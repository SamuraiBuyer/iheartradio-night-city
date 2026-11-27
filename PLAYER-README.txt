IHEART RADIO - Cyberpunk 2077 (v2.0.4)
======================================
Any iHeartRadio station as an in-game radio station - car radio, pocket radio and the radios in Night City -
with its logo redrawn in the style of the game's own station logos, and the song on air under NOW PLAYING.
Stations are added and removed with the Station Manager app: no reinstalling the mod.

REQUIREMENTS
- Cyberpunk 2077 2.x, Cyber Engine Tweaks, RED4ext
- RadioExt (nexusmods.com/cyberpunk2077/mods/4591)
- ffmpeg - the Station Manager installs it for you (button INSTALL FFMPEG), or: winget install Gyan.FFmpeg
- Windows 10/11 with the Edge WebView2 Runtime (built into Windows 11; Windows 10 usually has it)
- Internet connection while playing

INSTALL
Vortex / MO2: install "iHeart Radio 2.0.4.zip" like any other mod.
Manual: copy the "bin" and "red4ext" folders into your Cyberpunk 2077 folder.

ADD YOUR STATIONS - STATION MANAGER
Run  bin\x64\plugins\cyber_engine_tweaks\mods\iHeartRadio\StationManager\iHeartRadioManager.exe
  Vortex: Dashboard > Add Tool > pick that file, so it starts from Vortex.  MO2: add it as an executable.
1. ADD STATION, search by call letters, name, city or frequency (Z100, "102.7 KIIS FM", "New York rock").
2. Click the station. Name ("100.3 Z100"), frequency, stream and logo are filled in. The logo is converted
   automatically; pick another LOGO STYLE if one reads better, change LINE GAP / INVERT, or USE MY OWN IMAGE.
3. INSTALL STATION. Restart the game and tune in.
Click a station on the left to change its name, frequency, volume or logo, or to REMOVE it.
The game can stay open while you add stations: they show up next time it starts. The logos are written when the
game is closed (it keeps the logo file open) - the manager does it by itself as soon as you close the game.

HOW IT WORKS
- Stations: radioExt\radios\iheart_<id>_<name>\metadata.json (RadioExt web stream stations).
- Logos: archive\pc\mod\iHeartRadio_Stations.archive, written by the manager (512x512 BC7 textures + atlases,
  same setup as the game's radio icons, stored uncompressed). The logo pictures are kept in
  mods\iHeartRadio\data\logos.
- Audio: RadioExt can't decode iHeart's AAC audio, so iHeartRadio.dll (red4ext\plugins\iHeartRadio) runs a small
  bridge inside the game: it uses ffmpeg to turn each station into a plain MP3 stream on your own PC
  (http://127.0.0.1:8925/zc<station id>.mp3, not reachable from other computers). It stops with the game.
- Now playing: the bridge reads the song titles sent inside the stream and mods\iHeartRadio (CET) shows them.
Problems? red4ext\plugins\iHeartRadio\bridge.log (stations opened, songs, stream errors) and
mods\iHeartRadio\iHeartRadio.log (what was shown in game).

UNINSTALL
First open the Station Manager and click REMOVE ALL STATIONS (bottom of the window): stations and logos are files
the manager wrote, so mod managers don't remove them. Then remove the mod.

SOURCE
src\manager  Station Manager (C# .NET Framework 4.8 + WebView2; build.bat needs Visual Studio's C# compiler)
             ui\logo-core.js  logo conversion; IconArchive.cs  game texture / atlas / .archive writer
             templates\  CR2W templates and the script that makes them (WolvenKit, developer only)
src\plugin   RED4ext plugin with the bridge (Main.cpp, Bridge.cpp, build.bat - Visual Studio C++)
package.ps1  builds the Station Manager and the release zips

CHANGELOG
2.0.4 - first release

Not affiliated with iHeartMedia or CD PROJEKT RED. Station names and logos belong to their owners.
Includes Microsoft Edge WebView2 SDK files (BSD license, StationManager\WebView2-LICENSE.txt).
