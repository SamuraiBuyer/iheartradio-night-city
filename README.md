# iHeartRadio - Real Stations in Night City

A Cyberpunk 2077 mod that adds real iHeartRadio stations to the in-game radio, with logos redrawn in the game's
style and the song on air under NOW PLAYING.

The mod itself is on Nexus Mods: search for **iHeartRadio - Real Stations in Night City**. It comes in two versions,
pick one:

- **In-game Station Manager**: stations are added in a window in the Cyber Engine Tweaks overlay. Nothing else to
  download. Also the version to use through CrossOver / Wine.
- **For the Station Manager app**: no in-game window; stations are managed with the Windows app from this
  repository's [Releases](../../releases/latest).

This repository holds the **Station Manager app** and the source code of the whole mod.

## Download the Station Manager app

Only for the *For the Station Manager app* version of the mod. Get **`iHeart Radio Station Manager <version>.zip`**
from [Releases](../../releases/latest). App and mod versions go together: use the 2.1 app with the 2.1 mod.

The Station Manager is a standalone app, not a mod: don't install it with Vortex.

1. Install the mod from Nexus Mods first (it needs Cyber Engine Tweaks, RED4ext and RadioExt).
2. Download the Station Manager zip, right-click it > **Extract All**, and extract it anywhere (for example your
   Documents folder).
3. Open the **iHeart Radio Station Manager** folder and run `iHeartRadioManager.exe`. If Windows says
   *"Windows protected your PC"*, click **More info > Run anyway** (that shows for new apps that aren't code-signed).
4. It finds Cyberpunk 2077 through Steam, GOG or Epic. If it can't, click **SELECT FOLDER** and pick the game folder.
5. Click **ADD STATION**, search for a station, pick it and click **INSTALL STATION**. Start the game and tune in.

Tip: in Vortex you can add it under *Dashboard > Add Tool* to start it from there.

Needs Windows 10 or 11 with the Microsoft Edge WebView2 Runtime (already part of Windows 11).

### Antivirus warnings

A few antivirus programs flag the Station Manager app as a false positive. It's a machine-learning guess that comes
up with new apps that aren't code-signed. The full source is here, so you can read or build it yourself, or use the
In-game Station Manager version of the mod, which has no .exe at all.

## What's in the repo

| Folder | What it is |
|---|---|
| `src/plugin` | RED4ext plugin `iHeartRadio.dll`. **Bridge**: plays the stations inside the game. It decodes each station (AAC with Windows' decoder, or the built-in fdk-aac under CrossOver / Wine; MP3 with minimp3) and hands it to RadioExt as Ogg Vorbis on `127.0.0.1:8925` (not reachable from other computers), and reads the song titles. **In-game Station Manager**: `Stations.cpp` (search, install, remove), `Logo.cpp` (logo conversion, a C++ port of `logo-core.js`), `IconArchive.cpp` (the game's logo archive), `Script.cpp` (the functions the CET window calls). |
| `src/plugin/vendor` | Submodules: RED4ext.SDK, fdk-aac, libogg, libvorbis, minimp3, stb. |
| `src/manager` | Station Manager app: C# (.NET Framework 4.8) + WebView2 UI. Searches iHeartRadio, converts logos (`ui/logo-core.js`) and writes the game's logo archive itself (`IconArchive.cs`). |
| `bin/.../mods/iHeartRadio/init.lua` | Cyber Engine Tweaks script: shows the song titles in game. |
| `bin/.../mods/iHeartRadio/menu.lua` | The In-game Station Manager window (CET overlay). |
| `package.ps1` | Builds everything and makes the release zips (both mod versions and the app). |

Both versions write the same station files, so players can switch between them.

## Building

Needs Visual Studio 2022 (or newer) with **Desktop development with C++** and the **.NET desktop** C# compiler.
RED4ext.SDK has very long file names, so turn on long paths for git first:

```powershell
git config --global core.longpaths true
git clone --recurse-submodules <this repo>
powershell -ExecutionPolicy Bypass -File package.ps1
```

Builds are reproducible: the same source gives byte-identical `iHeartRadio.dll` and `iHeartRadioManager.exe`.

## Credits

- [RadioExt](https://www.nexusmods.com/cyberpunk2077/mods/4591), [Cyber Engine Tweaks](https://www.nexusmods.com/cyberpunk2077/mods/107),
  [RED4ext](https://www.nexusmods.com/cyberpunk2077/mods/2380) and [RED4ext.SDK](https://github.com/wopss/RED4ext.SDK)
- Built into the plugin: [fdk-aac](https://github.com/mstorsjo/fdk-aac) (AAC decoding),
  [libogg and libvorbis](https://xiph.org) (Ogg Vorbis), [minimp3](https://github.com/lieff/minimp3) (MP3 decoding),
  [stb](https://github.com/nothings/stb) (logo pictures). Their licenses ship with the mod in
  `red4ext/plugins/iHeartRadio/THIRD-PARTY-NOTICES.txt`.
- Microsoft Edge WebView2 SDK (included with the app under its own license)

Not affiliated with iHeartMedia or CD PROJEKT RED. Station names, logos and streams belong to their owners.

License: MIT (see `LICENSE`). The libraries in `src/plugin/vendor` keep their own licenses.
