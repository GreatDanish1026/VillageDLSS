# Builds bin\VillageDLSS.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# Usage: .\build.ps1 [-Install] [-GameDir <folder of re8.exe>]
#   -Install  copies the add-on and nvngx_dlss.dll next to re8.exe (game closed).
param(
    [switch]$Install,
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
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" /I `"$root\third_party\dlss\include`" " +
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
