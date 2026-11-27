<#
Builds the RED4ext plugin and the Station Manager, then the release zips for Vortex / MO2 / manual install.
  powershell -ExecutionPolicy Bypass -File package.ps1
Needs Visual Studio with the C++ tools and the C# compiler, and the RED4ext.SDK submodule
(git submodule update --init).
Output, with bin\ and red4ext\ like the game folder (the logo archive in archive\pc\mod is written on the player's
PC). The mod comes in two versions with the same plugin; players pick one:
  "iHeart Radio <version> - In-game Station Manager.zip"    stations are managed in the CET overlay (menu.lua)
  "iHeart Radio <version> - for the Station Manager app.zip" no in-game menu: stations are managed with the app
  "iHeart Radio Station Manager <version>.zip"              the app on its own (for GitHub), in an "iHeart Radio
                                                            Station Manager" folder: unzip anywhere and run (not a
                                                            Vortex mod - Vortex's Cyberpunk installer only accepts .dll
                                                            files in RED4ext plugins)
#>
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$version = ([regex]::Match((Get-Content "$root\src\manager\Backend.cs" -Raw), 'Version = "([^"]+)"')).Groups[1].Value
$tar = "$env:SystemRoot\System32\tar.exe"
$skip = @("--exclude", "*.log", "--exclude", "nowplaying.txt*", "--exclude", "logos", "--exclude", "logos_pending.txt")

# 1. RED4ext plugin -> red4ext\plugins\iHeartRadio\iHeartRadio.dll
& "$env:SystemRoot\System32\cmd.exe" /c "`"$root\src\plugin\build.bat`""
if ($LASTEXITCODE -ne 0) { throw "plugin build failed" }
New-Item -ItemType Directory -Force "$root\red4ext\plugins\iHeartRadio" | Out-Null
Copy-Item "$root\src\plugin\build\iHeartRadio.dll" "$root\red4ext\plugins\iHeartRadio\iHeartRadio.dll" -Force
# licenses of the code built into the DLL (fdk-aac requires its full license text next to the binary)
$vendor = "$root\src\plugin\vendor"
$notices = @("Third-party code built into iHeartRadio.dll. Source: https://github.com/SamuraiBuyer/iheartradio-night-city (src/plugin/vendor)", "")
foreach ($lib in @(
        @("Fraunhofer FDK AAC Codec Library for Android (fdk-aac 2.0.3, unmodified) - AAC decoding", "fdk-aac\NOTICE"),
        @("libogg 1.3.5 - Ogg container", "ogg\COPYING"),
        @("libvorbis 1.3.7 - Vorbis encoding", "vorbis\COPYING"),
        @("minimp3 - MP3 decoding", "minimp3\LICENSE"),
        @("stb_image 2.30 / stb_image_write 1.16 - station logo pictures (MIT or public domain)", "stb\LICENSE"),
        @("RED4ext.SDK", "RED4ext.SDK\LICENSE.md"))) {
    $notices += ("=" * 100), $lib[0], ("=" * 100), (Get-Content "$vendor\$($lib[1])" -Raw), ""
}
[IO.File]::WriteAllText("$root\red4ext\plugins\iHeartRadio\THIRD-PARTY-NOTICES.txt", ($notices -join "`r`n"))

# 2. Station Manager app
& "$env:SystemRoot\System32\cmd.exe" /c "`"$root\src\manager\build.bat`""
if ($LASTEXITCODE -ne 0) { throw "Station Manager build failed" }

# 3. zips (forward slashes inside, as mod managers expect)
$ingame = "$root\iHeart Radio $version - In-game Station Manager.zip"
$forApp = "$root\iHeart Radio $version - for the Station Manager app.zip"
$app = "$root\iHeart Radio Station Manager $version.zip"
foreach ($z in $ingame, $forApp, $app) { if (Test-Path $z) { Remove-Item $z } }
Push-Location $root
try { & $tar -a -c -f $ingame @skip --exclude "StationManager" bin red4ext }
finally { Pop-Location }
# the app version: the same files without menu.lua, and init.lua switched to "no overlay" (so a menu.lua left behind
# by the other download can't bring it back)
$modStage = Join-Path $env:TEMP "iheart-forapp-zip"
if (Test-Path $modStage) { Remove-Item $modStage -Recurse -Force }
New-Item -ItemType Directory $modStage | Out-Null
& $tar -x -f $ingame -C $modStage --exclude "*/menu.lua"
$init = "$modStage\bin\x64\plugins\cyber_engine_tweaks\mods\iHeartRadio\init.lua"
$text = [IO.File]::ReadAllText($init)
if ($text -notmatch 'local IN_GAME_MANAGER = true') { throw "init.lua: IN_GAME_MANAGER switch not found" }
[IO.File]::WriteAllText($init, $text.Replace('local IN_GAME_MANAGER = true', 'local IN_GAME_MANAGER = false'))
Push-Location $modStage
try { & $tar -a -c -f $forApp bin red4ext }
finally { Pop-Location; Remove-Item $modStage -Recurse -Force }
$stage = Join-Path $env:TEMP "iheart-app-zip"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null
Copy-Item "$root\src\manager\build\StationManager" "$stage\iHeart Radio Station Manager" -Recurse
Push-Location $stage
try { & $tar -a -c -f $app "iHeart Radio Station Manager" }
finally { Pop-Location; Remove-Item $stage -Recurse -Force }
foreach ($z in $ingame, $forApp, $app) {
    Write-Host "-> $z"
    & $tar -t -f $z | Where-Object { $_ -notmatch '/$' } | ForEach-Object { "     $_" }
}
