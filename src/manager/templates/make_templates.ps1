<#
Developer step, run once: rebuilds the two CR2W templates the Station Manager stamps station logos from.
  iheart_00000000.xbm.main   CBitmapTexture 512x512 (BC7), texture setup copied from the game's radio station icons;
                             only its main part (the pixel buffer is written by the manager)
  iheart_00000000.inkatlas   inkTextureAtlas with 3 slots -> iheartradio\icons\iheart_00000000.xbm, one full-size part
Needs WolvenKit.CLI, the game and node. The 8 zeros are replaced by the station id at run time (same length, so only
checksums change - see IconArchive.cs).
#>
param(
    [Parameter(Mandatory)][string]$WolvenKit,     # ...\WolvenKit.CLI.exe
    [Parameter(Mandatory)][string]$GameDir        # ...\Cyberpunk 2077
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$name = "iheart_00000000"
$work = Join-Path $env:TEMP "iheart_templates"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
$vanilla, $raw, $pack, $json = "$work\vanilla", "$work\raw", "$work\pack", "$work\json"
New-Item -ItemType Directory -Force $vanilla, "$raw\iheartradio\icons", "$pack\iheartradio\icons", $json | Out-Null

& $WolvenKit extract "$GameDir\archive\pc\content\basegame_1_engine.archive" -o $vanilla -r "radiostations_icons(\.xbm|\.inkatlas)$" | Out-Null
$v = "$vanilla\base\gameplay\gui\common\icons"
& $WolvenKit convert s "$v\radiostations_icons.inkatlas" -o $json | Out-Null
& $WolvenKit convert s "$v\radiostations_icons.xbm" -o $json | Out-Null

# a blank 512x512 picture: the texture size comes from it
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 512, 512, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bmp.Save("$raw\iheartradio\icons\$name.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()

$dst = "$pack\iheartradio\icons"
Copy-Item "$v\radiostations_icons.xbm" "$dst\$name.xbm"
& $WolvenKit import $raw --keep -o $pack | Out-Null          # exits with 3 even when it worked
& $WolvenKit convert s "$dst\$name.xbm" -o $json | Out-Null
node "$here\fix_xbm.js" "$json\$name.xbm.json" "$json\radiostations_icons.xbm.json"
Copy-Item "$json\$name.xbm.json" "$dst\$name.xbm.json"
& $WolvenKit convert d "$dst\$name.xbm.json" -o $dst | Out-Null
node "$here\make_atlas.js" "$json\radiostations_icons.inkatlas.json" "$dst\$name.inkatlas.json" "iheartradio\icons\$name.xbm" $name
& $WolvenKit convert d "$dst\$name.inkatlas.json" -o $dst | Out-Null

$x = [IO.File]::ReadAllBytes("$dst\$name.xbm")
$objectsEnd = [BitConverter]::ToUInt32($x, 24)
[IO.File]::WriteAllBytes("$here\$name.xbm.main", $x[0..($objectsEnd - 1)])
Copy-Item "$dst\$name.inkatlas" "$here\$name.inkatlas" -Force
Write-Host "templates written: $name.xbm.main ($objectsEnd bytes), $name.inkatlas"
