# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
# khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
#
# Makes the folder (and the zip) to give to people who want to play: the program, everything it needs to build the player's
# OWN code library from their OWN copy of the game with nothing to install (a portable Python; the compiler is downloaded
# once, checked against a SHA-256, on the first start), the licenses, the instructions and the complete corresponding source
# code the GPL requires. It never contains anything of the game: it is checked at the end.
#
#   powershell -ExecutionPolicy Bypass -File windows\package.ps1 [-Mode release|test] [-OutDir <dir>] [-BuildDir <dir>]
#
# Mode release (the default): the program as a player gets it. Mode test: the same, with the frame-rate counter on, a
# performance log and the helper files to send results back.
param(
    [ValidateSet("release", "test")][string]$Mode = "release",
    [string]$OutDir = "",
    [string]$BuildDir = ""
)
$ErrorActionPreference = "Stop"
$source = $PSScriptRoot
$repo = Split-Path $source -Parent
$parent = Split-Path $repo -Parent
if (-not $OutDir) { $OutDir = Join-Path $parent "dist\package" }
if (-not $BuildDir) { $BuildDir = Join-Path $parent "build\win" }
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")

# the portable Python (the embeddable package of python.org; PSF license): downloaded once into deps\, kept by its SHA-256
$pythonVersion = "3.12.8"
$pythonUrl = "https://www.python.org/ftp/python/$pythonVersion/python-$pythonVersion-embed-amd64.zip"
$pythonSha256 = "8D3F33BE9EB810F23C102F08475AF2854E50484B8E4E06275E937BE61CE3D2FB"  # recorded at the first download
$pythonZip = Join-Path $parent "deps\python-$pythonVersion-embed-amd64.zip"
if (-not (Test-Path $pythonZip)) {
    New-Item -ItemType Directory -Force (Split-Path $pythonZip -Parent) | Out-Null
    Write-Host "downloading $pythonUrl"
    Invoke-WebRequest -Uri $pythonUrl -OutFile $pythonZip
}
if ((Get-FileHash $pythonZip -Algorithm SHA256).Hash -ne $pythonSha256) { throw "$pythonZip does not have the expected SHA-256" }

$name = "Caravan4URecomp-windows-" + (Get-Date -Format "yyyyMMdd") + $(if ($Mode -eq "test") { "-test" } else { "" })
$pkg = Join-Path $OutDir $name
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force $pkg | Out-Null

# 1. the program, the DLLs, the licenses and a settings file (build.ps1 makes them)
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $source "build.ps1") -BuildDir $BuildDir -DistDir $pkg
if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed" }

# 2. the two tools the setup needs besides the program (they come from the same build)
$cmake = (Get-Command cmake).Source
& $cmake --build $BuildDir --target mh4u_native_runner mh4n_vkprobe_tool
if ($LASTEXITCODE -ne 0) { throw "building the runner and the probe failed" }
$tools = Join-Path $pkg "game-tools"
$bin = Join-Path $tools "bin"
New-Item -ItemType Directory -Force $bin | Out-Null
Copy-Item (Join-Path $BuildDir "bin\mh4u_native_runner.exe") $bin -Force
Copy-Item (Join-Path $BuildDir "bin\mh4n_vkprobe.exe") $bin -Force
$mingwBin = Split-Path (Get-Command clang).Source -Parent
$strip = Join-Path $mingwBin "llvm-strip.exe"
if (Test-Path $strip) {  # the debug information makes each program about 180 MB; the player does not need it
    foreach ($exe in (Join-Path $pkg "Caravan4URecomp.exe"), (Join-Path $bin "mh4u_native_runner.exe"), (Join-Path $bin "mh4n_vkprobe.exe")) {
        & $strip --strip-all $exe
    }
}
# the runner and the probe load the same runtime DLLs
foreach ($dll in "libc++.dll", "libunwind.dll", "libwinpthread-1.dll") { Copy-Item (Join-Path $pkg $dll) $bin -Force }

# 3. the portable Python that runs the recompiler, with its license
$py = Join-Path $tools "python"
Expand-Archive $pythonZip $py -Force
Copy-Item (Join-Path $py "LICENSE.txt") (Join-Path $pkg "LICENSES\PYTHON-LICENSE.txt") -Force

# 4. the recompiler and its tables (python, no game data: "They hold addresses, not game code")
foreach ($d in "tools", "config") {
    $from = Join-Path $repo "mh4n/$d"
    $to = Join-Path $tools $d
    # (not "android" by name: the recompiler's runtime folder has an android folder with headers the compiler needs;
    # only the top-level tools/android, the Android player's scripts, stays out)
    $androidScripts = Join-Path $repo "mh4n/tools/android"
    & robocopy $from $to /E /XD __pycache__ $androidScripts /NFL /NDL /NJH /NJS /NP | Out-Null
}

# 5. the instructions, the setup script and the source offer (and, in a test package, the helper files)
Copy-Item (Join-Path $source "package\*") $pkg -Force -Recurse
Remove-Item (Join-Path $pkg "1-PrepareGame.bat") -Force -ErrorAction SilentlyContinue  # the program asks for the files itself
if ($Mode -eq "release") {
    foreach ($f in "2-GpuCheck.bat", "perf_summary.py", "LEEME-PRUEBA.txt") { Remove-Item (Join-Path $pkg $f) -Force -ErrorAction SilentlyContinue }
} else {
    $ini = Join-Path $pkg "Caravan4URecomp.ini"
    $lines = Get-Content $ini | Where-Object { $_ -notmatch '^\s*(fps_counter|options)\s*=' }
    $lines += "fps_counter = on"
    $lines += 'options = --perf-log "%EXEDIR%\Caravan4URecomp-perf.csv"'
    $lines | Set-Content $ini -Encoding ascii
}

# 6. the complete corresponding source: the tree these binaries were built from, without version-control data and build output
$src = Join-Path $pkg "SOURCE"
New-Item -ItemType Directory -Force $src | Out-Null
$tmp = Join-Path $env:TEMP ("c4u-source-" + [guid]::NewGuid().ToString("N"))
& robocopy $repo (Join-Path $tmp "Caravan4URecomp") /E /XD .git __pycache__ build .gradle /NFL /NDL /NJH /NJS /NP | Out-Null
Compress-Archive -Path (Join-Path $tmp "Caravan4URecomp") -DestinationPath (Join-Path $src "Caravan4URecomp-source.zip") -Force
Remove-Item $tmp -Recurse -Force

# 7. nothing of the game: the checks that must never fail
$bad = Get-ChildItem $pkg -Recurse -File | Where-Object {
    $_.Extension -in ".cia", ".3ds", ".cci", ".cxi", ".3dsx", ".romfs" -or
    $_.Name -in "code.bin", "exefs.bin", "exheader.bin", "romfs.bin", "icon.bin" -or
    $_.Name -like "*_native.dll" -or $_.Name -like "*_native.so"
}
if ($bad) { Remove-Item $pkg -Recurse -Force; throw ("game files found in the package: " + ($bad.FullName -join ", ")) }
$zipBad = & powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::OpenRead('$((Join-Path $src 'Caravan4URecomp-source.zip'))').Entries | Where-Object { `$_.Name -match '\.(cia|3ds|cci|cxi)$' -or `$_.Name -in 'code.bin','exefs.bin','exheader.bin','romfs.bin' -or `$_.Name -like '*_native.dll' } | ForEach-Object { `$_.FullName }"
if ($zipBad) { Remove-Item $pkg -Recurse -Force; throw ("game files found in the source zip: " + ($zipBad -join ", ")) }

# 8. one zip to send
$zip = Join-Path $OutDir ($name + ".zip")
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $pkg -DestinationPath $zip -CompressionLevel Optimal
# the SHA-256 of the zip, to publish beside it (the exe is not code-signed: users can check the download with this)
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
Set-Content -Path ($zip + ".sha256") -Value ($hash + "  " + (Split-Path $zip -Leaf)) -Encoding ascii
Write-Host ("sha256: " + $hash)
Write-Host ("done: " + $zip + "  (" + [math]::Round((Get-Item $zip).Length / 1MB, 1) + " MB)")
