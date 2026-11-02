# Builds bin\VillageDLSS.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# Usage: .\build.ps1 [-Install] [-Package] [-GameDir <folder of re8.exe>]
#   -Install  copies the add-on, nvngx_dlss.dll and the REFramework script into the game folder
#             (game closed).
#   -Package  makes bin\release\VillageDLSS-<version>.zip for Fluffy Mod Manager: one folder,
#             VillageDLSS, with modinfo.ini and the files laid out as they go in the game folder.
param(
    [switch]$Install,
    [switch]$Package,
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Resident Evil Village BIOHAZARD VILLAGE"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars64.bat runs vswhere by name
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\VillageDLSS.addon64"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0.
$cmd = @(
    "call `"$vcvars`" >nul",
    "fxc /nologo /T vs_5_0 /E VsMain /O3 /Vn kBlitVs /Fh `"$obj\blit_vs.h`" `"$root\src\blit.hlsl`" >nul",
    "fxc /nologo /T ps_5_0 /E PsMain /O3 /Vn kBlitPs /Fh `"$obj\blit_ps.h`" `"$root\src\blit.hlsl`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$obj`" /I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" /I`"$root\third_party\dlss\include`" " +
     "/Fo`"$obj\\`" `"$root\src\main.cpp`" `"$root\src\upscaler.cpp`" `"$root\src\dlss.cpp`" `"$root\third_party\reshade\utils\descriptor_tracking.cpp`" " +
     "user32.lib advapi32.lib ole32.lib d3d12.lib `"$root\third_party\dlss\lib\nvsdk_ngx_s.lib`" " +
     "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\VillageDLSS.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

if ($Install) {
    if (-not (Test-Path "$GameDir\re8.exe")) { throw "re8.exe not found in $GameDir (pass -GameDir)" }
    if (Get-Process re8 -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item $addon $GameDir -Force
    # NVIDIA's signed DLSS runtime (310.9.16, from the Streamline 2.14.16 SDK's bin\x64; not in git).
    $ngx = "$root\third_party\bin\nvngx_dlss.dll"
    if (-not (Test-Path $ngx)) { throw "nvngx_dlss.dll not found in third_party\bin" }
    Copy-Item $ngx $GameDir -Force
    # REFramework companion script: sets the render scale for the DLSS mode dropdown.
    if (Test-Path "$GameDir\reframework") {
        New-Item -ItemType Directory -Force "$GameDir\reframework\autorun" | Out-Null
        Copy-Item "$root\reframework\VillageDLSS.lua" "$GameDir\reframework\autorun" -Force
    } else { Write-Warning "REFramework not installed: the DLSS mode dropdown needs it" }
    Write-Host "Installed to $GameDir"
}

if ($Package) {
    $version = [regex]::Match((Get-Content "$root\src\version.h" -Raw), 'RD_VERSION "([^"]+)"').Groups[1].Value
    $ngx = "$root\third_party\bin\nvngx_dlss.dll"
    if (-not (Test-Path $ngx)) { throw "nvngx_dlss.dll not found in third_party\bin" }
    $stage = "$root\bin\release\VillageDLSS-$version"
    $mod = "$stage\VillageDLSS"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force "$mod\reframework\autorun", "$mod\VillageDLSS-licenses" | Out-Null

    # Files installed into the game folder (where re8.exe is).
    Copy-Item $addon, $ngx $mod
    Copy-Item "$root\reframework\VillageDLSS.lua" "$mod\reframework\autorun"
    Copy-Item "$root\README.md" "$mod\VillageDLSS-README.md"
    Copy-Item "$root\THIRD_PARTY_NOTICES.txt" "$mod\VillageDLSS-THIRD_PARTY_NOTICES.txt"
    Copy-Item "$root\LICENSE" "$mod\VillageDLSS-licenses\VillageDLSS-LICENSE.txt"
    Copy-Item "$root\third_party\dlss\LICENSE.txt" "$mod\VillageDLSS-licenses\NVIDIA-DLSS-LICENSE.txt"
    Copy-Item "$root\third_party\reshade\LICENSE.md" "$mod\VillageDLSS-licenses\ReShade-LICENSE.md"

    # Fluffy Mod Manager's mod information (key=value, one per line).
    @(
        "name=VillageDLSS - DLSS for Resident Evil Village",
        "version=v$version",
        "description=Replaces the game's TAA with NVIDIA DLSS: DLAA, or DLSS upscaling from Quality to Ultra Performance with the game's bloom, tonemapping (RenoDX HDR works), sharpening and HUD on top at full resolution. Needs an NVIDIA RTX card, ReShade 6.8+ with add-on support, and REFramework for the DLSS mode dropdown. Settings: ReShade overlay (Home), Add-ons tab, VillageDLSS; F9 switches DLSS on and off.",
        "author=Scott Dinwiddie"
    ) | Set-Content "$mod\modinfo.ini" -Encoding ascii

    # Zip with forward-slash entry names (Compress-Archive in PowerShell 5.1 writes backslashes,
    # which some tools mishandle).
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = "$root\bin\release\VillageDLSS-$version.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        Get-ChildItem $stage -Recurse -File | ForEach-Object {
            $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
    Write-Host "Packaged $zip"
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        "{0,-60} {1,12:N0}" -f $_.FullName.Substring($stage.Length + 1), $_.Length
    }
}
