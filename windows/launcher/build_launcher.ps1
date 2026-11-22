# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
#
# Builds Caravan4URecomp-Launcher.exe (PyInstaller, one file) from windows\launcher\ and copies it to -OutDir.
#   powershell -ExecutionPolicy Bypass -File windows\launcher\build_launcher.ps1 -OutDir <folder> [-Version 0.4.0]
# Needs Python 3 with PySide6-Essentials, Pillow and PyInstaller (pip install PySide6-Essentials Pillow pyinstaller).
param([Parameter(Mandatory)][string]$OutDir, [string]$Version = "")
$ErrorActionPreference = "Stop"
$src = $PSScriptRoot
$work = Join-Path ([IO.Path]::GetTempPath()) ("c4u-launcher-build-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    Copy-Item (Join-Path $src "*.py") $work
    Copy-Item (Join-Path $src "c4u_version_info.txt") $work
    Copy-Item (Join-Path $src "Caravan4URecomp-Launcher.spec") $work
    New-Item -ItemType Directory -Force (Join-Path $work "assets") | Out-Null
    Copy-Item (Join-Path $src "assets\caravan4u.ico") (Join-Path $work "assets")
    Copy-Item (Join-Path $src "assets\header.jpg") (Join-Path $work "assets")
    if ($Version) {
        $parts = ($Version -split '\.') + @("0", "0", "0", "0") | Select-Object -First 4
        $quad = ($parts -join ".")
        $comma = ($parts -join ", ")
        $vi = Join-Path $work "c4u_version_info.txt"
        (Get-Content $vi -Raw) -replace '\(0, 4, 0, 0\)', "($comma)" -replace '0\.4\.0\.0', $quad | Set-Content $vi -Encoding utf8
    }
    Push-Location $work
    & python -m PyInstaller --noconfirm --clean --distpath (Join-Path $work "dist") --workpath (Join-Path $work "build") "Caravan4URecomp-Launcher.spec"
    if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }
    Pop-Location
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    Copy-Item (Join-Path $work "dist\Caravan4URecomp-Launcher.exe") $OutDir -Force
    Write-Host "launcher: $(Join-Path $OutDir 'Caravan4URecomp-Launcher.exe')"
} finally {
    Set-Location $src
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
