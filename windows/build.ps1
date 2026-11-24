# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
# khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
# Builds mh4u-win for Windows x64 with llvm-mingw (clang), CMake and Ninja, and assembles a folder to run it from.
#
#   powershell -ExecutionPolicy Bypass -File windows\build.ps1 [-BuildDir <dir>] [-DistDir <dir>] [-Sdl3Dir <dir>]
#
# Needs on the PATH (or found by this script): cmake (3.25 or newer), ninja, and llvm-mingw's clang/clang++ (x86_64,
# UCRT; winget: MartinStorsjo.LLVM-MinGW.UCRT). SDL3 (3.4.18 or newer, the MinGW development package) is downloaded
# from the official release page when -Sdl3Dir is not given. The game itself is not built here: see
# mh4n/tools/build_game.py (the code library) and the notes in the project's docs.
#
# Nothing is written inside the source tree: the build and the dist folder go where the parameters say (by default
# next to the repository, in ..\build\win and ..\dist\mh4u-win).
param(
    [string]$BuildDir = "",
    [string]$DistDir = "",
    [string]$Sdl3Dir = "",
    [string]$Sdl3Version = "3.4.20",
    [switch]$Launcher,  # also build Caravan4URecomp-Launcher.exe (PyInstaller) into the dist folder
    [switch]$NoFsr,     # a diagnostic build: FSR compiled out (the upscaler setting is ignored)
    [switch]$Release    # a build to give to other people: no path of this PC in the program (see below)
)
$ErrorActionPreference = "Stop"

$source = $PSScriptRoot                                   # <repo>\windows
$repo = Split-Path $source -Parent
$parent = Split-Path $repo -Parent
if (-not $BuildDir) { $BuildDir = Join-Path $parent "build\win" }
if (-not $DistDir) { $DistDir = Join-Path $parent "dist\mh4u-win" }

# the user's and the machine's PATH (a shell opened before an install does not have the new entries)
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")

function Need($name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if (-not $c) { throw "$name is not on the PATH." }
    return $c.Source
}
$cmake = Need "cmake"
$ninja = Need "ninja"
$clang = Need "clang"
$null = Need "clang++"
$mingwBin = Split-Path $clang -Parent
if (-not (Test-Path (Join-Path $mingwBin "libc++.dll"))) { throw "clang is not llvm-mingw's (no libc++.dll beside it): $clang" }
Write-Host "cmake: $cmake`nninja: $ninja`nclang: $clang"

# SDL3
if (-not $Sdl3Dir) {
    $Sdl3Dir = Join-Path $parent "deps\sdl3-$Sdl3Version"
}
$sdlPrefix = Join-Path $Sdl3Dir "SDL3-$Sdl3Version\x86_64-w64-mingw32"
if (-not (Test-Path (Join-Path $sdlPrefix "lib\cmake\SDL3\SDL3Config.cmake"))) {
    New-Item -ItemType Directory -Force $Sdl3Dir | Out-Null
    $zip = Join-Path $Sdl3Dir "SDL3-devel-$Sdl3Version-mingw.zip"
    $url = "https://github.com/libsdl-org/SDL/releases/download/release-$Sdl3Version/SDL3-devel-$Sdl3Version-mingw.zip"
    Write-Host "downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip
    Expand-Archive -Path $zip -DestinationPath $Sdl3Dir -Force
}
if (-not (Test-Path (Join-Path $sdlPrefix "bin\SDL3.dll"))) { throw "SDL3 not found under $sdlPrefix" }

# configure and build
# -include cstdlib: the vendored {fmt} 11.2.0 lacks the include with a recent libc++ (llvm-mingw 2025 and later)
# A release is for other people: the compiler writes the path of each source file into the program (the log's file:line, asserts) and
# those paths would be this PC's (the user's name, the folders). -ffile-prefix-map rewrites them: the user's profile folder becomes
# "<home>", the tree becomes "." (so a log line reads ./4u-native/mh4n/... ). The dist folder for testing keeps the real paths.
$mapFlags = ""
# FSR is out of every build (it did not help on AMD nor NVIDIA GPUs, and cost frames): the upscaler setting is ignored
$mapFlags = " -DC4U_NO_FSR"
# a test build (not -Release) reads the GPU time of each frame for the log; the release does not (C4U_GPU_TIME=1 turns it on there)
if (-not $Release) { $mapFlags = $mapFlags + " -DC4U_GPU_TIME_LOG" }
if ($Release) {
    $maps = @()
    $home1 = $env:USERPROFILE
    foreach ($p in @($home1, $home1.Replace('\', '/'))) { $maps += "-ffile-prefix-map=$p=HOME" }   # the general one first: the last match wins
    foreach ($p in @($parent, $parent.Replace('\', '/'))) { $maps += "-ffile-prefix-map=$p=." }
    $mapFlags = $mapFlags + " " + ($maps -join " ")
    Write-Host "release build: source paths are rewritten"
}
& $cmake -S $source -B $BuildDir -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
    "-DCMAKE_CXX_FLAGS=-include cstdlib$mapFlags" "-DCMAKE_C_FLAGS=$($mapFlags.Trim())" "-DCMAKE_PREFIX_PATH=$sdlPrefix"
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $cmake --build $BuildDir --target mh4u-win mh4n_hdpack_tool
if ($LASTEXITCODE -ne 0) { throw "build failed" }

# the folder to run it from: the exe, SDL3 and llvm-mingw's runtime DLLs; the settings file is kept if there is one
New-Item -ItemType Directory -Force $DistDir | Out-Null
Copy-Item (Join-Path $BuildDir "Caravan4URecomp.exe") $DistDir -Force
Copy-Item (Join-Path $sdlPrefix "bin\SDL3.dll") $DistDir -Force
# the HD texture converter the launcher runs (mh4n_hdpack.exe, in game-tools\bin as in the package)
New-Item -ItemType Directory -Force (Join-Path $DistDir "game-tools\bin") | Out-Null
Copy-Item (Join-Path $BuildDir "bin\mh4n_hdpack.exe") (Join-Path $DistDir "game-tools\bin") -Force
foreach ($dll in "libc++.dll", "libunwind.dll", "libwinpthread-1.dll") {
    Copy-Item (Join-Path $mingwBin $dll) $DistDir -Force
}
# the licenses, the notices and the credits the project's additional terms keep (ADDITIONAL-TERMS a): beside the program
$lic = Join-Path $DistDir "LICENSES"
New-Item -ItemType Directory -Force $lic | Out-Null
$sdlRoot = Join-Path $Sdl3Dir "SDL3-$Sdl3Version"
$copies = @(
    @((Join-Path $repo "LICENSE"), "LICENSE-GPL-3.0.txt"),
    @((Join-Path $repo "ADDITIONAL-TERMS"), "ADDITIONAL-TERMS.txt"),
    @((Join-Path $repo "THIRD_PARTY_NOTICES"), "THIRD_PARTY_NOTICES.txt"),
    @((Join-Path $source "THIRD_PARTY_NOTICES_WINDOWS"), "THIRD_PARTY_NOTICES_WINDOWS.txt"),
    @((Join-Path $repo "mh4n\native\THIRD_PARTY.md"), "THIRD_PARTY-azahar-derived-files.md"),
    @((Join-Path $repo "README.md"), "README.md"),
    @((Join-Path $repo "README.es.md"), "README.es.md"),
    @((Join-Path $sdlRoot "LICENSE.txt"), "SDL3-LICENSE.txt"),
    @((Join-Path $mingwBin "..\LICENSE.TXT"), "LLVM-LICENSE.txt"),
    @((Join-Path $source "licenses\winpthreads-COPYING.txt"), "winpthreads-COPYING.txt"),
    @((Join-Path $source "licenses\Cinzel-OFL.txt"), "Cinzel-OFL.txt"),
    @((Join-Path $source "licenses\LICENSE-FidelityFX-FSR.txt"), "LICENSE-FidelityFX-FSR.txt")
)
foreach ($c in $copies) {
    if (Test-Path $c[0]) { Copy-Item $c[0] (Join-Path $lic $c[1]) -Force } else { Write-Warning "license file not found: $($c[0])" }
}
# the licence texts of the libraries built into the program (plume, volk, VMA, ENet, fmt, glslang, ImGui, LodePNG, xxHash, bc7enc,
# inih, CityHash, Vulkan headers, the system archives and their fonts): the tree already keeps them for the Android app
$third = Join-Path $lic "third-party"
New-Item -ItemType Directory -Force $third | Out-Null
$thirdSrc = Join-Path $repo "android\app\src\main\assets\licenses"
foreach ($n in "plume","volk","VulkanMemoryAllocator","Vulkan-Headers","ENet","fmt","glslang","DearImGui","LodePNG","xxHash","bc7enc","inih","CityHash","SystemArchives","Fonts","HIDAPI") {
    $f = Join-Path $thirdSrc "$n.txt"
    if (Test-Path $f) { Copy-Item $f (Join-Path $third "$n.txt") -Force } else { Write-Warning "license text not found: $f" }
}
$ini = Join-Path $DistDir "Caravan4URecomp.ini"
if (-not (Test-Path $ini)) {
    @"
# Caravan4URecomp (MH4U for Windows): settings. Command-line options override these. Delete a line to use its default.
# game = <the game folder made by mh4u_native_runner --from-dump>
# code = <the code library made by mh4n\tools\build_game.py --native>
# user = <a folder for saves and caches>
scale = 3
volume = 50
vsync = on
fullscreen = off
display = -1
skip_key = Tab
circle_pad_pro = on
fps_counter = off
"@ | Set-Content $ini -Encoding ascii
}
if ($Launcher) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $source ("launcher" + [IO.Path]::DirectorySeparatorChar + "build_launcher.ps1")) -OutDir $DistDir $(if ($NoFsr) { "-NoFsr" })
    if ($LASTEXITCODE -ne 0) { throw "build_launcher.ps1 failed" }
}
Write-Host "done: $DistDir\Caravan4URecomp.exe"
