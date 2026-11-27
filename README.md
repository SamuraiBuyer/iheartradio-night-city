# iHeartRadio - Real Stations in Night City

A Cyberpunk 2077 mod that adds real iHeartRadio stations to the in-game radio, with logos redrawn in the game's
style and the song on air under NOW PLAYING.

This repository holds the **Station Manager** app (the download players need) and the source code of the whole mod.
The mod itself is on Nexus Mods: search for **iHeartRadio - Real Stations in Night City**.

## Download the Station Manager

Get **`iHeart Radio Station Manager <version>.zip`** from [Releases](../../releases/latest).

The Station Manager is a standalone app, not a mod: don't install it with Vortex.

1. Install the mod from Nexus Mods first (it needs Cyber Engine Tweaks, RED4ext and RadioExt).
2. Download the Station Manager zip, right-click it > **Extract All**, and extract it anywhere (for example your
   Documents folder).
3. Open the **iHeart Radio Station Manager** folder and run `iHeartRadioManager.exe`. If Windows says
   *"Windows protected your PC"*, click **More info > Run anyway** (that shows for new apps that aren't code-signed).
4. It finds Cyberpunk 2077 through Steam, GOG or Epic. If it can't, click **SELECT FOLDER** and pick the game folder.
5. Click **ADD STATION**, search for a station, pick it and click **INSTALL STATION**. Start the game and tune in.

Tip: in Vortex you can add it under *Dashboard > Add Tool* to start it from there.

Needs Windows 10 or 11 with the Microsoft Edge WebView2 Runtime (already part of Windows 11). The app can install
ffmpeg for you, which the mod uses to play the stations.

### Antivirus warnings

A few antivirus programs flag the Station Manager as a false positive. It's a machine-learning guess that comes up
with new apps that aren't code-signed. Windows Defender and the major scanners report it clean. The full source is
here, so you can read or build it yourself.

## What's in the repo

| Folder | What it is |
|---|---|
| `src/manager` | Station Manager: C# (.NET Framework 4.8) + WebView2 UI. Searches iHeartRadio, converts logos (`ui/logo-core.js`) and writes the game's logo archive itself (`IconArchive.cs`). |
| `src/plugin` | RED4ext plugin `iHeartRadio.dll`. Runs a small local bridge inside the game that uses ffmpeg to turn a station into an MP3 stream on `127.0.0.1:8925` (not reachable from other computers), and reads the song titles. |
| `bin/.../mods/iHeartRadio/init.lua` | Cyber Engine Tweaks script that shows the song titles in game. |
| `package.ps1` | Builds everything and makes the release zips. |

## Building

Needs Visual Studio 2022 (or newer) with **Desktop development with C++** and the **.NET desktop** C# compiler.

```powershell
git clone --recurse-submodules <this repo>
powershell -ExecutionPolicy Bypass -File package.ps1
```

Builds are reproducible: the same source gives byte-identical `iHeartRadio.dll` and `iHeartRadioManager.exe`.

## Credits

- [RadioExt](https://www.nexusmods.com/cyberpunk2077/mods/4591), [Cyber Engine Tweaks](https://www.nexusmods.com/cyberpunk2077/mods/107),
  [RED4ext](https://www.nexusmods.com/cyberpunk2077/mods/2380) and [RED4ext.SDK](https://github.com/wopss/RED4ext.SDK)
- [FFmpeg](https://ffmpeg.org) (not included, installed separately)
- Microsoft Edge WebView2 SDK (included under its own license)

Not affiliated with iHeartMedia or CD PROJEKT RED. Station names, logos and streams belong to their owners.

License: MIT (see `LICENSE`).
