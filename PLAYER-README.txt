IHEART RADIO - Cyberpunk 2077 (v2.1.0)
======================================
Any iHeartRadio station as an in-game radio station - car radio, pocket radio and the radios in Night City -
with its logo redrawn in the style of the game's own station logos, and the song on air under NOW PLAYING.
Stations are added and removed with the Station Manager: no reinstalling the mod.

TWO VERSIONS - PICK ONE
- In-game Station Manager: the Station Manager is a window in the Cyber Engine Tweaks overlay. Nothing else to
  download. Also works through CrossOver / Wine.
- For the Station Manager app: no in-game menu; stations are managed with the Windows app (separate download from
  GitHub, below). It has the bigger logo preview and USE MY OWN IMAGE.
Both play the stations the same way, and both make the same station files, so you can switch later.

REQUIREMENTS
- Cyberpunk 2077 2.x, Cyber Engine Tweaks, RED4ext
- RadioExt (nexusmods.com/cyberpunk2077/mods/4591)
- Internet connection while playing
- App version only: Windows 10/11 with the Edge WebView2 Runtime (built into Windows 11)

INSTALL
Vortex / MO2: install the mod's zip from Nexus Mods like any other mod.
Manual: copy the "bin" and "red4ext" folders into your Cyberpunk 2077 folder.

ADD YOUR STATIONS - IN-GAME (In-game Station Manager version)
Open the Cyber Engine Tweaks overlay (the key you picked when CET started) and find the "iHeart Radio" window.
1. ADD STATION, search by station name, call letters or city (Z100, Los Angeles), click a station.
2. Name, frequency and logo are filled in; the preview shows the logo. Pick another logo style if one reads
   better, or change INVERT / LINE GAP.
3. INSTALL STATION, then restart the game and tune in. Logos go in at the restart too (the game keeps its logo
   file open while it runs).
Click a station under MY STATIONS to change it or REMOVE it.

ADD YOUR STATIONS - STATION MANAGER APP (version for the Station Manager app)
The Station Manager is a standalone app (not a mod), downloaded separately:
  https://github.com/SamuraiBuyer/iheartradio-night-city/releases/latest
Extract the zip anywhere (e.g. Documents) and run "iHeart Radio Station Manager\iHeartRadioManager.exe".
If Windows shows "Windows protected your PC": More info > Run anyway (new apps that aren't code-signed).
It finds the game through Steam, GOG or Epic; otherwise click SELECT FOLDER and pick the Cyberpunk 2077 folder.
  Vortex: Dashboard > Add Tool > pick the exe, so it starts from Vortex.  MO2: add it as an executable.
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
  bridge inside the game: it decodes the station (Windows' own AAC decoder, or the AAC decoder built into the
  plugin where Windows has none and under CrossOver / Wine) and hands it to RadioExt as Ogg Vorbis on your own PC
  (http://127.0.0.1:8925/zc<station id>.ogg, not reachable from other computers). It stops with the game.
  Nothing else is installed or started. Stations made with 2.0.x are switched to the .ogg address automatically.
- Now playing: the bridge reads the song titles sent inside the stream and mods\iHeartRadio (CET) shows them.
Problems? red4ext\plugins\iHeartRadio\bridge.log (stations opened, songs, stream errors) and
mods\iHeartRadio\iHeartRadio.log (what was shown in game).

UNINSTALL
First click REMOVE ALL STATIONS (in-game: under MY STATIONS; app: bottom of the window): stations and logos are
files the Station Manager wrote, so mod managers don't remove them. Then remove the mod.

SOURCE
src\manager  Station Manager (C# .NET Framework 4.8 + WebView2; build.bat needs Visual Studio's C# compiler)
             ui\logo-core.js  logo conversion; IconArchive.cs  game texture / atlas / .archive writer
             templates\  CR2W templates and the script that makes them (WolvenKit, developer only)
src\plugin   RED4ext plugin (build.bat - Visual Studio C++): Bridge.cpp the stream bridge; Stations.cpp,
             Logo.cpp, IconArchive.cpp, Script.cpp the in-game Station Manager (menu.lua is its CET window)
             vendor\  fdk-aac, libogg, libvorbis, minimp3, stb, RED4ext.SDK (git submodule update --init)
package.ps1  builds the plugin and the Station Manager app, and the release zips

CHANGELOG
2.1.0 - Station Manager inside the game (CET overlay), as its own download next to the app version
        no more ffmpeg: the plugin decodes the stations itself and sends Ogg Vorbis to the game
        (the same build works through CrossOver / Wine)
2.0.4 - first release

Not affiliated with iHeartMedia or CD PROJEKT RED. Station names and logos belong to their owners.
The Station Manager app includes Microsoft Edge WebView2 SDK files (BSD license, WebView2-LICENSE.txt in its folder).
iHeartRadio.dll includes fdk-aac, libogg, libvorbis and minimp3: red4ext\plugins\iHeartRadio\THIRD-PARTY-NOTICES.txt.
