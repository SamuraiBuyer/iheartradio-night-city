<#
Builds the RED4ext plugin and the Station Manager, then the release zips for Vortex / MO2 / manual install.
  powershell -ExecutionPolicy Bypass -File package.ps1
Needs Visual Studio with the C++ tools and the C# compiler, and the RED4ext.SDK submodule
(git submodule update --init).
Output, with bin\ and red4ext\ like the game folder (the logo archive in archive\pc\mod is written by the Station
Manager on the player's PC):
  "iHeart Radio <version>.zip"                          everything
  "iHeart Radio <version> (no Station Manager).zip"     the mod without the app
  "iHeart Radio Station Manager <version>.zip"          the app on its own, same game-folder layout, so Vortex / MO2
                                                        install it into the game like a mod
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

# 2. Station Manager -> bin\x64\plugins\cyber_engine_tweaks\mods\iHeartRadio\StationManager
& "$env:SystemRoot\System32\cmd.exe" /c "`"$root\src\manager\build.bat`""
if ($LASTEXITCODE -ne 0) { throw "Station Manager build failed" }
$mods = "$root\bin\x64\plugins\cyber_engine_tweaks\mods\iHeartRadio"
if (Test-Path "$mods\StationManager") { Remove-Item "$mods\StationManager" -Recurse -Force }
Copy-Item "$root\src\manager\build\StationManager" "$mods\StationManager" -Recurse

# 3. zips (forward slashes inside, as mod managers expect)
$full = "$root\iHeart Radio $version.zip"
$core = "$root\iHeart Radio $version (no Station Manager).zip"
$app = "$root\iHeart Radio Station Manager $version.zip"
foreach ($z in $full, $core, $app) { if (Test-Path $z) { Remove-Item $z } }
Push-Location $root
try {
    & $tar -a -c -f $full @skip bin red4ext
    & $tar -a -c -f $core @skip --exclude "StationManager" bin red4ext
    & $tar -a -c -f $app "bin/x64/plugins/cyber_engine_tweaks/mods/iHeartRadio/StationManager"
}
finally { Pop-Location }
foreach ($z in $full, $core, $app) {
    Write-Host "-> $z"
    & $tar -t -f $z | Where-Object { $_ -notmatch '/$' } | ForEach-Object { "     $_" }
}
