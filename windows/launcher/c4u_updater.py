# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# Adapted from the update code of the author's other launcher project (updater.py, make_release.py).

"""File-by-file updates for the Caravan4URecomp Windows launcher."""

import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timedelta

UPDATE_REPO = "G30vamv/Caravan4URecomp"
RELEASE_URL = f"https://github.com/{UPDATE_REPO}/releases"
MANIFEST_URL = f"{RELEASE_URL}/latest/download/manifest.json"
MANIFEST_ENV = "C4U_UPDATE_MANIFEST"
LAUNCHER_EXE = "Caravan4URecomp-Launcher.exe"
UPDATE_DIR = "_update"
BACKUP_DIR = "_update_backup"
BACKUP_MARKER = "completed.json"
ASSET_SIZE_LIMIT = 2 * 1024**3
ASSET_NAME_LIMIT = 255

PROTECTED = {"caravan4urecomp.ini", "launcher_config.json", "*.log", "*.log.old"}
PROTECTED_DIRS = {"game", "user", "toolchain", "python", UPDATE_DIR, BACKUP_DIR, "crash", "backups"}
PACKAGED_DIRS = {"python", "toolchain"}
ALLOWED_EXT = {".exe", ".dll", ".py", ".pyd", ".ps1", ".bat", ".txt", ".png", ".ico", ".json", ".md", ".zip", ".ttf",
                ".h", ".c", ".tsv", ".cat", ".pth", "._pth"}

_SEMVER = re.compile(
    r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
)
_SHA256 = re.compile(r"[0-9a-fA-F]{64}")
_DEVICE = re.compile(r"(?:con|prn|aux|nul|com[0-9¹²³]|lpt[0-9¹²³])", re.IGNORECASE)
_BAD_CHARS = re.compile(r'[\x00-\x1f<>:"\\|?*~]')


class UpdateError(Exception):
    def __init__(self, code, message_en, message_es):
        self.code = code
        self.message_en = message_en
        self.message_es = message_es
        super().__init__(message_en)


@dataclass
class UpdateInfo:
    version: str
    notes_en: str
    notes_es: str
    files: list
    total_bytes: int
    needs_launcher_restart: bool
    _previous: dict = field(default_factory=dict, init=False, repr=False)
    _stage: Path | None = field(default=None, init=False, repr=False)


def parse_version(text) -> tuple:
    """Return a SemVer precedence key, also accepting v and windows-v prefixes."""
    version = str(text).removeprefix("windows-").removeprefix("v")
    match = _SEMVER.fullmatch(version)
    if not match:
        raise UpdateError("version", f"Invalid version: {text}", f"Versión no válida: {text}")
    prerelease = match[4]
    identifiers = []
    for part in prerelease.split(".") if prerelease else ():
        if part.isdigit():
            if len(part) > 1 and part.startswith("0"):
                raise UpdateError("version", f"Invalid version: {text}", f"Versión no válida: {text}")
            identifiers.append((0, int(part)))
        else:
            identifiers.append((1, part))
    return int(match[1]), int(match[2]), int(match[3]), prerelease is None, tuple(identifiers)


def release_tag(version):
    parse_version(version)
    if not _SEMVER.fullmatch(version):
        raise UpdateError("version", f"Expected SemVer without a prefix: {version}",
                          f"Se esperaba SemVer sin prefijo: {version}")
    return f"windows-v{version}"


def _path_error(path):
    return UpdateError("path", f"Disallowed update path: {path}",
                       f"Ruta de actualización no permitida: {path}")


def _windows_key(text):
    return text.upper().casefold()


def safe_path(path):
    """Validate the Windows relative path used by both publisher and updater."""
    if not isinstance(path, str) or _BAD_CHARS.search(path):
        raise _path_error(path)
    parts = path.split("/")
    for part in parts:
        if not part or part.endswith((".", " ")) or _DEVICE.fullmatch(part.split(".")[0].rstrip()):
            raise _path_error(path)
    protected = {_windows_key(name) for name in PROTECTED}
    dirs = {_windows_key(name) for name in PROTECTED_DIRS} - PACKAGED_DIRS
    lower = [_windows_key(part) for part in parts]
    if any(part in protected or ("*.log" in protected and part.endswith(".log"))
           or ("*.log.old" in protected and part.endswith(".log.old")) for part in lower):
        raise _path_error(path)
    if (any(part in dirs for part in lower)
            or Path(parts[-1]).suffix.casefold() not in ALLOWED_EXT):
        raise _path_error(path)
    return path


def asset_name(path):
    name = "f-" + safe_path(path).encode("utf-8").hex()
    if len(name) > ASSET_NAME_LIMIT:
        raise UpdateError("asset_name", f"Asset name exceeds {ASSET_NAME_LIMIT} characters: {path}",
                          f"El nombre del asset supera {ASSET_NAME_LIMIT} caracteres: {path}")
    return name


def _reparse(result):
    return (stat.S_ISLNK(result.st_mode)
            or getattr(result, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def owned_path(root, relative):
    """Resolve a child lexically, rejecting symlinks and Windows reparse points."""
    target = root
    for component in Path(relative).parts:
        target /= component
        try:
            if _reparse(target.lstat()):
                raise _path_error(relative)
        except FileNotFoundError:
            pass
    return target


GAME_PROGRAMS = ("caravan4urecomp.exe", "mh4u_native_runner.exe")


def running_games(base_dir) -> list:
    """The names of the game programs of THIS installation that are running, however they were started (found by the full path
    of each process: a copy of the game in another folder is not ours). Nothing is ever closed by the updater."""
    if os.name != "nt":
        return []
    import ctypes
    from ctypes import wintypes

    class Entry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("pid", wintypes.DWORD),
                    ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                    ("name", wintypes.WCHAR * 260)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(Entry))
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    own = os.path.normcase(os.path.realpath(base_dir)).rstrip("\\") + "\\"
    found = []
    snapshot = kernel.CreateToolhelp32Snapshot(0x2, 0)  # the processes
    if snapshot in (None, wintypes.HANDLE(-1).value):
        return found
    try:
        entry = Entry(size=ctypes.sizeof(Entry))
        more = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if entry.name.casefold() in GAME_PROGRAMS:
                process = kernel.OpenProcess(0x1000, False, entry.pid)  # only to ask where it runs from
                if process:
                    buffer = ctypes.create_unicode_buffer(32768)
                    length = wintypes.DWORD(len(buffer))
                    if (kernel.QueryFullProcessImageNameW(process, 0, buffer, ctypes.byref(length))
                            and os.path.normcase(os.path.realpath(buffer.value)).startswith(own)):
                        found.append(entry.name)
                    kernel.CloseHandle(process)
            more = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return found


def _game_open_error():
    return UpdateError("running", "The game is open. Close it and update again.",
                       "El juego está abierto. Ciérralo y vuelve a actualizar.")


def sha256_file(path):
    with open(path, "rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def _invalid_manifest():
    return UpdateError("manifest", "Invalid update manifest.", "Manifiesto de actualización no válido.")


def _validate_url(url, *, testing):
    if not isinstance(url, str) or any(ord(char) <= 32 for char in url):
        raise _invalid_manifest()
    try:
        parsed = urllib.parse.urlsplit(url)
        if (parsed.scheme in ({"http", "https"} if testing else {"https"})
                and parsed.hostname and parsed.username is None and parsed.password is None):
            return
        if testing and url.startswith("file:///") and not parsed.netloc and not parsed.path.startswith("//"):
            local = urllib.request.url2pathname(parsed.path)
            if not local.startswith(("//", "\\\\")):
                return
    except ValueError:
        pass
    raise _invalid_manifest()


def _validate_files(files, *, testing):
    if not isinstance(files, list):
        raise _invalid_manifest()
    paths = set()
    directories = set()
    for entry in files:
        if not isinstance(entry, dict):
            raise _invalid_manifest()
        path = safe_path(entry.get("path"))
        key = _windows_key(path)
        parents = {_windows_key(parent.as_posix()) for parent in Path(path).parents if parent != Path(".")}
        if key in paths or key in directories or parents & paths:
            raise _path_error(path)
        paths.add(key)
        directories.update(parents)
        digest, size = entry.get("sha256"), entry.get("size")
        if (not isinstance(digest, str) or not _SHA256.fullmatch(digest)
                or type(size) is not int or not 0 <= size < ASSET_SIZE_LIMIT):
            raise _invalid_manifest()
        _validate_url(entry.get("url"), testing=testing)


def validate_manifest(data, *, testing=False):
    if not isinstance(data, dict):
        raise _invalid_manifest()
    for key in ("version", "tag", "released", "notes_en", "notes_es", "min_launcher"):
        if not isinstance(data.get(key), str):
            raise _invalid_manifest()
    if data["tag"] != release_tag(data["version"]):
        raise _invalid_manifest()
    try:
        released = datetime.fromisoformat(data["released"])
    except ValueError as error:
        raise _invalid_manifest() from error
    if released.utcoffset() != timedelta(0):
        raise _invalid_manifest()
    parse_version(data["min_launcher"])
    package = data.get("package_zip")
    if (not isinstance(package, dict)
            or package.get("name") != f'Caravan4URecomp-{data["version"]}-windows-x64.zip'
            or not isinstance(package.get("sha256"), str) or not _SHA256.fullmatch(package["sha256"])):
        raise _invalid_manifest()
    _validate_files(data.get("files"), testing=testing)
    return data


def _with_retries(action):
    for attempt in range(3):
        try:
            return action()
        except urllib.error.HTTPError as error:
            if attempt == 2 or (error.code < 500 and error.code not in (408, 429)):
                raise
        except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.IncompleteRead):
            if attempt == 2:
                raise
        time.sleep(2 ** (attempt + 1))


def _open(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Caravan4URecomp-Launcher-Updater"})
    return urllib.request.urlopen(request, timeout=60)


def load_manifest(url, *, testing=False):
    _validate_url(url, testing=testing)

    def read():
        with _open(url) as source:
            return json.load(source)

    try:
        data = _with_retries(read)
    except (OSError, ValueError, http.client.HTTPException) as error:
        raise UpdateError("manifest_read", f"Could not read the update manifest: {error}",
                          f"No se pudo leer el manifiesto de actualización: {error}") from error
    return validate_manifest(data, testing=testing)


def fetch_manifest() -> dict:
    override = os.environ.get(MANIFEST_ENV)
    return load_manifest(override or MANIFEST_URL, testing=bool(override))


def _inspect(root, relative):
    path = owned_path(root, relative)
    if path.is_file():
        return sha256_file(path)
    if path.exists():
        raise _path_error(relative)
    return None


def check(current_version: str, base_dir: str) -> UpdateInfo | None:
    manifest = fetch_manifest()
    current = parse_version(current_version)
    if parse_version(manifest["version"]) <= current:
        return None
    if parse_version(manifest["min_launcher"]) > current:
        raise UpdateError("min_launcher", f'Launcher {manifest["min_launcher"]} or newer is required.',
                          f'Se necesita el launcher {manifest["min_launcher"]} o posterior.')
    base = Path(base_dir).resolve()
    files = []
    previous = {}
    try:
        for entry in manifest["files"]:
            path = entry["path"]
            digest = _inspect(base, path)
            if digest != entry["sha256"].lower():
                files.append(entry)
                previous[path] = digest
    except OSError as error:
        raise UpdateError("inspect", f"Could not inspect the installation: {error}",
                          f"No se pudo revisar la instalación: {error}") from error
    info = UpdateInfo(manifest["version"], manifest["notes_en"], manifest["notes_es"], files,
                      sum(entry["size"] for entry in files),
                      any(entry["path"].casefold() == LAUNCHER_EXE.casefold() for entry in files))
    info._previous = previous
    return info


def download(info, base_dir, progress=None) -> str:
    _validate_files(info.files, testing=bool(os.environ.get(MANIFEST_ENV)))
    stage = None
    done = 0
    total = info.total_bytes or 1
    try:
        base = Path(base_dir).resolve(strict=True)
        updates = owned_path(base, UPDATE_DIR)
        updates.mkdir(exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix="apply-", dir=updates))
        for entry in info.files:
            relative = entry["path"]
            dest = stage / "files" / relative
            dest.parent.mkdir(parents=True, exist_ok=True)

            def fetch():
                count = 0
                digest = hashlib.sha256()
                with _open(entry["url"]) as source, dest.open("wb") as output:
                    while chunk := source.read(1 << 18):
                        count += len(chunk)
                        if count > entry["size"]:
                            break
                        digest.update(chunk)
                        output.write(chunk)
                        if progress:
                            progress(min((done + count) / total, 1.0), relative)
                return count == entry["size"] and digest.hexdigest() == entry["sha256"].lower()

            if not _with_retries(fetch):
                raise UpdateError("integrity", f"Size or SHA-256 mismatch: {relative}",
                                  f"El tamaño o SHA-256 no coincide: {relative}")
            done += entry["size"]
        if progress and not done:
            progress(1.0, info.files[-1]["path"] if info.files else "")
    except BaseException as error:
        if stage is not None:
            shutil.rmtree(stage, ignore_errors=True)
        if isinstance(error, (OSError, http.client.HTTPException)):
            raise UpdateError("download", f"Could not download the update: {error}",
                              f"No se pudo descargar la actualización: {error}") from error
        raise
    info._stage = stage
    return str(stage)


def clean_env():
    """Start the next PyInstaller bootloader with a fresh extraction environment."""
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith("_PYI_") and key.upper() != "_MEIPASS2"}
    env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    return env


_BATCH = r"""@echo off
setlocal DisableDelayedExpansion
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0transaction.ps1"
exit /b %errorlevel%
"""

_TRANSACTION = r"""
$ErrorActionPreference = 'Stop'
$stage = $PSScriptRoot
$updates = Split-Path -Parent $stage
$base = Split-Path -Parent $updates
$plan = Get-Content -LiteralPath (Join-Path $stage 'plan.json') -Raw -Encoding UTF8 | ConvertFrom-Json

function Get-OwnedPath([string] $root, [string] $relative) {
    foreach ($part in $relative.Split('/')) {
        $root = [IO.Path]::Combine($root, $part)
        try {
            if ([IO.File]::GetAttributes($root) -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point / Punto de reanalisis: $root"
            }
        } catch [IO.FileNotFoundException], [IO.DirectoryNotFoundException] {}
    }
    return $root
}

function Get-Digest([string] $path) {
    if ([IO.Directory]::Exists($path)) { throw "Expected a file / Se esperaba un archivo: $path" }
    if ([IO.File]::Exists($path)) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    return $null
}

function Move-File([string] $source, [string] $destination) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
    try { [IO.File]::Move($source, $destination) }
    catch { throw "Move failed / Fallo al mover $($source): $($_.Exception.Message)" }
}

function Wait-Launcher([int] $launcherPid) {
    $launcher = Get-Process -Id $launcherPid -ErrorAction SilentlyContinue
    if ($launcher -and -not $launcher.WaitForExit(120000)) {
        throw 'Launcher did not close / El launcher no se cerro.'
    }
}

# A game is never closed by force: it may be writing a save. The launcher asks the player to close it before it gets here;
# one opened since then is waited for a moment, and if it stays open nothing is changed. Only the games of this folder count.
function Wait-GameClosed {
    $own = $base.TrimEnd('\') + '\'
    for ($try = 0; $try -lt 20; $try++) {
        $open = @(Get-Process -Name 'Caravan4URecomp', 'mh4u_native_runner' -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -and $_.Path.StartsWith($own, [StringComparison]::OrdinalIgnoreCase) })
        if ($open.Count -eq 0) { return }
        Start-Sleep -Milliseconds 500
    }
    throw 'The game is open: close it and update again / El juego esta abierto: cierralo y vuelve a actualizar.'
}

function Remove-EmptyDirectories($directories) {
    foreach ($directory in $directories) {
        try { [IO.Directory]::Delete($directory) } catch [IO.IOException] {}
    }
}

$lock = $null
$result = 1
$committed = $false
$snapshotCreated = $false
$started = $false
$journal = [Collections.Generic.List[object]]::new()
$newDirs = [Collections.Generic.List[string]]::new()
try {
    Wait-Launcher $plan.launcher_pid
    $null = Get-OwnedPath $base ('_update/' + [IO.Path]::GetFileName($stage))
    $lockPath = Get-OwnedPath $updates 'transaction.lock'
    $lock = [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None')
    $started = $true
    $backupRoot = Get-OwnedPath $base '_update_backup'
    $snapshot = Get-OwnedPath $backupRoot $plan.snapshot
    $marker = Get-OwnedPath $snapshot 'completed.json'
    if ($plan.undo) {
        $latest = Get-ChildItem -LiteralPath $backupRoot -Directory -Filter 'apply-*' |
            Where-Object { -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } |
            ForEach-Object { Get-Item -LiteralPath (Join-Path $_.FullName 'completed.json') -ErrorAction SilentlyContinue } |
            Sort-Object LastWriteTimeUtc, FullName -Descending | Select-Object -First 1
        if (-not $latest -or $latest.FullName -ne $marker) {
            throw 'Backup is no longer current / La copia de seguridad ya no es la actual.'
        }
    } else {
        if ([IO.Directory]::Exists($snapshot)) { throw 'Backup exists / La copia de seguridad ya existe.' }
        [void][IO.Directory]::CreateDirectory($snapshot)
        $snapshotCreated = $true
    }

    if ($plan.live) { Wait-GameClosed }
    foreach ($entry in $plan.entries) {
        $path = Get-OwnedPath $base $entry.path
        $backup = Get-OwnedPath $snapshot ('files/' + $entry.path)
        if ($plan.undo) {
            $source = $backup
            $hold = Get-OwnedPath $stage ('undo/' + $entry.path)
            $oldDigest = $entry.sha256
            $newDigest = $entry.before
        } else {
            $source = Get-OwnedPath $stage ('files/' + $entry.path)
            $hold = $backup
            $oldDigest = $entry.before
            $newDigest = $entry.sha256
        }
        if ((Get-Digest $path) -ne $oldDigest -or (Get-Digest $source) -ne $newDigest) {
            throw "File changed / El archivo cambio: $($entry.path)"
        }
        $journal.Add(@{ path = $path; source = $source; hold = $hold; saved = $false;
                         installed = $false; readonly = $false; replace = ($null -ne $newDigest);
                         existed = ($null -ne $oldDigest) })
    }

    foreach ($change in $journal) {
        if ($change.existed -and $change.replace) {
            # one step (ReplaceFile): the new file takes the old one's place and the old one goes to the backup. There is no
            # moment without a file there, so a power cut cannot leave the folder without its launcher.
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($change.hold))
            try {
                # Replace refuses a read-only file (a zip tool or a copy from a disc can leave one)
                $info = [IO.FileInfo]::new($change.path)
                if ($info.IsReadOnly) { $info.IsReadOnly = $false; $change.readonly = $true }
                [IO.File]::Replace($change.source, $change.path, $change.hold)
            }
            catch { throw "Replace failed / Fallo al reemplazar $($change.path): $($_.Exception.Message)" }
            $change.saved = $true
            $change.installed = $true
            continue
        }
        if ($change.existed) {
            Move-File $change.path $change.hold
            $change.saved = $true
        }
        if ($change.replace) {
            $parent = [IO.Path]::GetDirectoryName($change.path)
            while (-not [IO.Directory]::Exists($parent)) {
                $newDirs.Add($parent)
                $parent = [IO.Path]::GetDirectoryName($parent)
            }
            Move-File $change.source $change.path
            $change.installed = $true
        }
    }

    if ($plan.undo) {
        Move-File $marker (Join-Path $stage 'undone.json')
    } else {
        $plan | Add-Member -NotePropertyName 'created_dirs' -NotePropertyValue @(
            $newDirs | ForEach-Object { $_.Substring($base.Length + 1).Replace('\', '/') }
        )
        $pendingMarker = Join-Path $stage 'completed.json'
        [IO.File]::WriteAllText($pendingMarker, ($plan | ConvertTo-Json -Depth 6))
        Move-File $pendingMarker $marker
    }
    $committed = $true
    $result = 0
} catch {
    [Console]::Error.WriteLine("Update failed / Fallo de actualizacion: " + $_.Exception.Message)
    $restoreFailed = $false
    for ($index = $journal.Count - 1; $index -ge 0; $index--) {
        $change = $journal[$index]
        try {
            if ($change.installed) { Move-File $change.path $change.source }
            if ($change.saved) { Move-File $change.hold $change.path }
            if ($change.readonly) { [IO.FileInfo]::new($change.path).IsReadOnly = $true }
        } catch {
            $restoreFailed = $true
            [Console]::Error.WriteLine("Restore failed / Fallo al restaurar: " + $_.Exception.Message)
        }
    }
    Remove-EmptyDirectories ($newDirs | Sort-Object Length -Descending -Unique)
    if ($snapshotCreated -and -not $restoreFailed) {
        try { [IO.Directory]::Delete($snapshot, $true) } catch [IO.IOException] {}
    }
    if ($restoreFailed) { $started = $false }
} finally {
    try {
        if ($committed) {
            if ($plan.undo) {
                $directories = $plan.created_dirs | ForEach-Object { Get-OwnedPath $base $_ }
                Remove-EmptyDirectories ($directories | Sort-Object Length -Descending)
                [IO.Directory]::Delete($snapshot, $true)
            }
            $payload = Join-Path $stage $(if ($plan.undo) { 'undo' } else { 'files' })
            if ([IO.Directory]::Exists($payload)) { [IO.Directory]::Delete($payload, $true) }
        }
    } catch {
        [Console]::Error.WriteLine("Cleanup failed / Fallo de limpieza: " + $_.Exception.Message)
    } finally {
        if ($lock) { $lock.Dispose() }
    }
}

if ($plan.live -and $started) {
    try {
        foreach ($item in Get-ChildItem Env:) {
            if ($item.Name -like '_PYI_*' -or $item.Name -eq '_MEIPASS2') {
                [Environment]::SetEnvironmentVariable($item.Name, $null)
            }
        }
        $env:PYINSTALLER_RESET_ENVIRONMENT = '1'
        Start-Process -FilePath (Join-Path $base 'Caravan4URecomp-Launcher.exe') -WorkingDirectory $base
    } catch {
        [Console]::Error.WriteLine("Launcher restart failed / Fallo al reabrir el launcher: " + $_.Exception.Message)
        exit 2
    }
}
exit $result
"""


def _write_transaction(stage, plan, filename):
    (stage / "plan.json").write_text(json.dumps(plan, ensure_ascii=False), encoding="utf-8")
    (stage / "transaction.ps1").write_text(_TRANSACTION, encoding="utf-8-sig")
    batch = stage / filename
    batch.write_text(_BATCH, encoding="ascii", newline="\r\n")
    return str(batch)


def _launch_script(batch, stage):
    system = Path(os.environ["SystemRoot"]) / "System32"
    flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
    with owned_path(stage.parent, "apply_update.log").open("wb") as log:
        command = str(Path(stage.name) / Path(batch).name)
        subprocess.Popen([str(system / "cmd.exe"), "/d", "/c", command],
                         cwd=stage.parent, creationflags=flags, close_fds=True, stdin=subprocess.DEVNULL,
                         stdout=log, stderr=subprocess.STDOUT, env=clean_env())


def apply(info, base_dir, relaunch=True) -> str:
    if os.name != "nt":
        raise UpdateError("platform", "Updates can only be applied on Windows.",
                          "Las actualizaciones solo se pueden aplicar en Windows.")
    try:
        base = Path(base_dir).resolve(strict=True)
        updates = owned_path(base, UPDATE_DIR)
        stage = info._stage
        if stage is None or stage.parent != updates or not owned_path(updates, stage.name).is_dir():
            raise UpdateError("staging", "Download the update before applying it.",
                              "Descarga la actualización antes de aplicarla.")
        _validate_files(info.files, testing=bool(os.environ.get(MANIFEST_ENV)))
        if relaunch and running_games(base):  # (opened while the update was downloading)
            raise _game_open_error()
        entries = []
        for entry in info.files:
            relative = entry["path"]
            before = (info._previous[relative] if relative in info._previous
                      else _inspect(base, relative))
            entries.append({"path": relative, "sha256": entry["sha256"], "before": before})
        entries.sort(key=lambda e: e["path"].casefold() == LAUNCHER_EXE.casefold())  # (the launcher last: it is what recovers)
        batch = _write_transaction(stage, {"launcher_pid": os.getpid(), "live": relaunch,
                                            "undo": False, "snapshot": stage.name, "entries": entries},
                                   "apply_update.bat")
        if relaunch:
            _launch_script(batch, stage)
        return batch
    except OSError as error:
        raise UpdateError("apply", f"Could not prepare the update: {error}",
                          f"No se pudo preparar la actualización: {error}") from error


def _digest_of(path):
    return sha256_file(path).lower() if path.is_file() else None


def _put_back(source, target, aside):
    """`source` takes `target`'s place; a program that is running (the launcher itself) can be moved away but not written over."""
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.replace(source, target)
    except PermissionError:
        aside.parent.mkdir(parents=True, exist_ok=True)
        os.replace(target, aside)
        os.replace(source, target)


def _recover_one(base, updates, snapshot):
    """One update that never finished. Returns (what happened, how many files were put back):
    "undone": the files from before it are back;  "complete": every new file was in place, so it is kept and marked finished;
    "stale": the folder has changed since (something else was installed over it), so nothing is touched;
    "lost": it cannot be undone (a file from before it is gone or damaged), so nothing is touched."""
    stage = owned_path(updates, snapshot.name)
    try:
        plan = json.loads((stage / "plan.json").read_text(encoding="utf-8"))
        entries = [(safe_path(e["path"]), e["sha256"].lower(), (e["before"] or "").lower() or None) for e in plan["entries"]]
    except (OSError, ValueError, TypeError, KeyError, AttributeError, UpdateError):
        if not any(p.is_file() for p in snapshot.rglob("*")):  # (nothing had been moved into it)
            shutil.rmtree(snapshot, ignore_errors=True)
            return "stale", 0
        return "lost", 0
    changes = []
    states = []
    for path, new, old in entries:
        target = owned_path(base, path)
        backup = owned_path(snapshot, "files/" + path)
        now, saved = _digest_of(target), _digest_of(backup)
        states.append((now, new))
        if saved is not None:  # the old file had been moved out
            if saved != old:
                changes.append(("lost", path, target, backup))
            elif now not in (None, new):
                changes.append(("stale", path, target, backup))
            else:
                changes.append(("restore", path, target, backup))
        elif now == old:
            continue  # not reached
        elif now == new:
            changes.append(("remove" if old is None else "lost", path, target, backup))
        else:
            changes.append(("stale", path, target, backup))
    if states and all(now == new for now, new in states):  # cut between the last file and the note that it had finished
        plan.setdefault("created_dirs", [])
        pending = snapshot / (BACKUP_MARKER + ".tmp")
        pending.write_text(json.dumps(plan, ensure_ascii=False), encoding="utf-8")
        os.replace(pending, snapshot / BACKUP_MARKER)
        return "complete", 0
    kinds = {kind for kind, *_ in changes}
    if "stale" in kinds:
        os.replace(snapshot, snapshot.with_name("stale-" + snapshot.name))  # (kept, and not looked at again)
        return "stale", 0
    if "lost" in kinds:
        return "lost", 0
    for kind, path, target, backup in reversed(changes):
        aside = stage / "discarded" / path
        if kind == "restore":
            _put_back(backup, target, aside)
        else:
            try:
                target.unlink()
            except PermissionError:
                aside.parent.mkdir(parents=True, exist_ok=True)
                os.replace(target, aside)
    shutil.rmtree(snapshot, ignore_errors=True)
    shutil.rmtree(stage / "files", ignore_errors=True)
    return "undone", len(changes)


def recover_interrupted(base_dir):
    """An update cut short (a power cut, the PC turned off) leaves a backup without the note that says it finished. Called when
    the launcher opens: the installation ends up whole, in one version. Returns None when there was nothing to do, else
    {"restored": files put back, "problem": None, "game_open" or "lost"}."""
    base = Path(base_dir).resolve()
    root = owned_path(base, BACKUP_DIR)
    if not root.is_dir():
        return None
    unfinished = [s for s in sorted(root.glob("apply-*"), reverse=True)
                  if s.is_dir() and not _reparse(s.lstat()) and not (s / BACKUP_MARKER).is_file()]
    if not unfinished:
        return None
    updates = owned_path(base, UPDATE_DIR)
    updates.mkdir(exist_ok=True)
    try:  # a transaction at work holds this file with no sharing
        lock = owned_path(updates, "transaction.lock").open("a+b")
    except OSError:
        return None
    result = {"restored": 0, "problem": None}
    with lock:
        if running_games(base):
            return {"restored": 0, "problem": "game_open"}
        for snapshot in unfinished:
            try:
                what, count = _recover_one(base, updates, snapshot)
            except (OSError, UpdateError):
                what, count = "lost", 0
            result["restored"] += count
            if what == "lost":
                result["problem"] = "lost"
    return result if result["restored"] or result["problem"] else None


def _backups(base):
    root = owned_path(base, BACKUP_DIR)
    if not root.exists():
        return []
    backups = []
    for snapshot in root.glob("apply-*"):
        if not snapshot.is_dir() or _reparse(snapshot.lstat()):
            continue
        marker = owned_path(snapshot, BACKUP_MARKER)
        if marker.is_file():
            backups.append((marker.stat().st_mtime_ns, snapshot))
    return [path for _, path in sorted(backups, reverse=True)]


def prune_backups(base_dir, keep=1) -> list:
    base = Path(base_dir).resolve()
    if not _backups(base)[max(0, keep):]:
        return []
    updates = owned_path(base, UPDATE_DIR)
    updates.mkdir(exist_ok=True)
    removed = []
    # A live Windows transaction opens this file with FileShare.None.
    try:
        lock = owned_path(updates, "transaction.lock").open("a+b")
    except OSError:
        return []
    with lock:
        for snapshot in _backups(base)[max(0, keep):]:
            try:
                shutil.rmtree(snapshot)
                removed.append(snapshot.name)
            except OSError:
                continue
    return removed


def rollback_script(base_dir) -> str | None:
    base = Path(base_dir).resolve()
    stage = None
    try:
        backups = _backups(base)
        if not backups:
            return None
        snapshot = backups[0]
        plan = json.loads((snapshot / BACKUP_MARKER).read_text(encoding="utf-8"))
        for entry in plan["entries"]:
            safe_path(entry["path"])
        for directory in plan["created_dirs"]:
            safe_path(f"{directory}/placeholder.txt")
        updates = owned_path(base, UPDATE_DIR)
        updates.mkdir(exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix="undo-", dir=updates))
        plan.update(undo=True, launcher_pid=os.getpid(), snapshot=snapshot.name)
        return _write_transaction(stage, plan, "rollback_update.bat")
    except (OSError, ValueError, TypeError, KeyError) as error:
        if stage is not None:
            shutil.rmtree(stage, ignore_errors=True)
        raise UpdateError("rollback", f"Could not prepare the rollback: {error}",
                          f"No se pudo preparar la reversión: {error}") from error
