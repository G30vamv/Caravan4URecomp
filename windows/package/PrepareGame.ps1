# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
# khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES in LICENSES.
#
# Builds YOUR code library from YOUR OWN decrypted copy of Monster Hunter 4 Ultimate (USA: the base game and the v1.3.0
# update) and writes Caravan4URecomp.ini so that Caravan4URecomp.exe starts the game. Caravan4URecomp.exe runs this on its
# first start. Nothing of the game is in this package and nothing built here must be shared: it stays on this PC (the "game"
# folder).
#
# It needs nothing installed: the Python that runs the recompiler comes with the package (game-tools\python), and the C
# compiler (llvm-mingw's clang, from the llvm-mingw project) is downloaded once into the game folder when the PC has none:
# only the 50 files that building needs (about 50 MB, fetch_toolchain.py, each checked against a SHA-256), or, if that does
# not work, the whole release (about 190 MB, checked against the SHA-256 of the zip).
#
#   Caravan4URecomp.exe                                       (first start: asks for the two files)
#   powershell -File PrepareGame.ps1 -Base <base .cia/.3ds/.cci> -Update <update .cia> [-Wait]
param(
    [string]$Base = "",
    [string]$Update = "",
    [switch]$Wait    # on a failure, wait for Enter so that the messages can be read (the program opens this in its own window)
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$tools = Join-Path $root "game-tools"
$out = Join-Path $root "game"

try {
    function Ask($prompt, $value) {
        if (-not $value) { $value = Read-Host $prompt }
        return $value.Trim().Trim('"')
    }
    $Base = Ask "Path of the decrypted BASE game (USA, .cia, .3ds or .cci)" $Base
    $Update = Ask "Path of the decrypted v1.3.0 UPDATE (USA, .cia)" $Update
    # (-LiteralPath everywhere in this script: to PowerShell, [ ] in a path are wildcards, and dumps are often named "... [USA].cia")
    if (-not (Test-Path -LiteralPath $Base -PathType Leaf)) { throw "not found: $Base" }
    if (-not (Test-Path -LiteralPath $Update -PathType Leaf)) { throw "not found: $Update" }

    # the Python: the one in the package, else one on the PATH
    $python = Join-Path $tools "python\python.exe"
    if (-not (Test-Path -LiteralPath $python)) {
        $found = Get-Command python -ErrorAction SilentlyContinue
        if (-not $found) { throw "no Python found (game-tools\python is missing from this package, and none is installed)." }
        $python = $found.Source
        & $python -c "import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)"
        if ($LASTEXITCODE -ne 0) { throw "Python 3.10 or newer is needed." }
    }

    # the C compiler: llvm-mingw's clang; when the PC has none, the part of it that building needs is downloaded once
    $fetch = @()
    if (-not (Get-Command x86_64-w64-mingw32-clang -ErrorAction SilentlyContinue)) {
        Write-Host "No C compiler found: the first time, the part of llvm-mingw that building needs is downloaded (about 50 MB; needs Internet)."
        $fetchTool = Join-Path $root "fetch_toolchain.py"
        if (Test-Path -LiteralPath $fetchTool) {
            & $python $fetchTool --dest (Join-Path $out "toolchain")
            if ($LASTEXITCODE -ne 0) { Write-Host "The small download did not work: the whole compiler is downloaded instead (about 190 MB)." }
        }
        # build_game.py uses the compiler in game\toolchain when it is there, and downloads the whole llvm-mingw when it is not
        $fetch = @("--fetch-compiler")
    }

    Write-Host "Building the code library for this PC (a few minutes)..."
    & $python (Join-Path $tools "tools\build_game.py") --dump $Update --romfs $Base --native --opt O1 --out $out @fetch
    if ($LASTEXITCODE -ne 0) { throw "the code library could not be built: read the messages above (a file that is encrypted, or that is not the USA v1.3.0 update, stops it here)." }

    $dll = Get-ChildItem -LiteralPath (Join-Path $out "recomp") -Filter "*_native.dll" | Select-Object -First 1
    if (-not $dll) { throw "the code library was not made" }

    # the C and the objects the library was made from are only a cache for making it again: the game runs from the library
    # alone, and they take more than 500 MB
    Remove-Item -LiteralPath (Join-Path $out "recomp\src"), (Join-Path $out "recomp\obj") -Recurse -Force -ErrorAction SilentlyContinue

    Write-Host "Preparing the game folder..."
    $runner = Join-Path $tools "bin\mh4u_native_runner.exe"
    & $runner (Join-Path $out "folder") --from-dump "$Base,$Update" --print-process | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "the game folder could not be made" }

    # the names of the monsters, read from this game's own files, for the life bar (not important: the bar works without them)
    & $python (Join-Path $tools "tools\monster_names.py") (Join-Path $out "folder") | Out-Null

    $user = Join-Path $out "user"
    [void][IO.Directory]::CreateDirectory($user)

    # the settings file: the three paths, the rest as it is
    $ini = Join-Path $root "Caravan4URecomp.ini"
    $lines = @()
    if (Test-Path -LiteralPath $ini) { $lines = @(Get-Content -LiteralPath $ini -Encoding UTF8) }
    function SetKey($key, $value) {
        # (@( ) at each step: with no line or one, PowerShell hands back a plain text, and += would glue the keys into one line)
        $script:lines = @($script:lines | ForEach-Object {
            if ($_ -match "^\s*#?\s*$key\s*=") { "$key = $value" } else { $_ }
        })
        if (-not @($script:lines | Where-Object { $_ -match "^\s*$key\s*=" }).Count) { $script:lines = @($script:lines) + "$key = $value" }
    }
    SetKey "game" (Join-Path $out "folder")
    SetKey "code" $dll.FullName
    SetKey "user" $user
    # UTF-8 without a byte order mark, as the game and the launcher write it: a folder or a nickname with accents stays as it is
    [IO.File]::WriteAllLines($ini, [string[]]@($lines), (New-Object Text.UTF8Encoding $false))
    Write-Host "Done."
    exit 0
}
catch {
    Write-Host ""
    Write-Host ("ERROR: " + $_.Exception.Message) -ForegroundColor Red
    if ($Wait) { Read-Host "Press Enter to close this window" | Out-Null }
    exit 1
}
