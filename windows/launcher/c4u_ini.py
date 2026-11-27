# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# Our own code, written from: windows/win_main.cpp (Ini and its option reads).

"""Read and write Caravan4URecomp.ini in the game's format."""

import contextlib
import os
import time
from pathlib import Path
from tempfile import NamedTemporaryFile

from c4u_options import OPTIONS


_GROUPS = (
    ("Game", ("game", "code", "user", "options")),
    ("Video", ("scale", "fullscreen", "width", "height", "display", "vsync",
               "fps", "steady", "effects", "aspect", "widescreen", "mode", "touch_show",
               "popup_size", "popup_corner", "hide_help", "antialiasing", "bloom", "motion_blur", "depth_of_field",
               "radial_blur", "haze", "dither", "fov", "fps_counter", "pace", "gpu_thread", "shader_fallback",
               "hd_textures", "hd_texture_max")),
    ("Audio", ("volume",)),
    ("Controls", ("kick", "target", "skip_end", "skip_end_hold", "skip_key", "skip_pad", "map_key", "map_pad", "circle_pad_pro", "mouse_camera",
                  "hid_pads", "hidhide_ask")),
    ("Online", ("nick", "join_address", "host_port", "lobby", "online_touch")),
    ("Mods", ("monster_hp", "monster_hp_size", "monster_hp_pos", "monster_hp_percent", "damage_meter", "damage_mult", "qol_hold_gather", "qol_flex_cancel", "qol_sharpen_cancel",
              "qol_rewards_no_limit", "rewards_send_all", "invincible", "inf_items", "hud_scale", "hud_parts", "hud_scale_clock", "hud_scale_bars", "hud_scale_sharpness",
              "hud_scale_party", "hud_scale_map", "hud_scale_target", "hud_scale_items")),
    ("Other", ("language", "discord_presence", "auto_backup", "trace_panels")),
)
_DEFAULTS = {option.key: option.default for option in OPTIONS}
_WHITESPACE = " \t\r\n"


def _lines(path: Path) -> list[str]:
    try:
        source = path.read_bytes().decode("utf-8-sig", errors="surrogateescape")
    except FileNotFoundError:
        return []
    lines = (line.strip(_WHITESPACE) for line in source.split("\n"))
    return [line for line in lines if line and not line.startswith("# -- ")]


def _entry(line: str) -> tuple[str, str] | None:
    if line.startswith("#"):
        return None
    key, separator, value = line.partition("=")
    key = key.strip(_WHITESPACE)
    if separator and key:
        return key, value.strip(_WHITESPACE)
    return None


def _retry(action):
    """An editor, an antivirus or the game can hold the file for a moment: tried for half a second, then the error goes out."""
    for attempt in range(10):
        try:
            return action()
        except PermissionError:
            if attempt == 9:
                raise
            time.sleep(0.05)


@contextlib.contextmanager
def lock():
    """One writer at a time: the game takes the same named mutex (win_main.cpp, Ini::FileLock)."""
    if os.name != "nt":
        yield
        return
    import ctypes
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateMutexW.restype = wintypes.HANDLE
    kernel.CreateMutexW.argtypes = (wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR)
    kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel.ReleaseMutex.argtypes = (wintypes.HANDLE,)
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    handle = kernel.CreateMutexW(None, False, "Local\\Caravan4URecomp.ini")
    try:
        if handle:
            kernel.WaitForSingleObject(handle, 3000)
        yield
    finally:
        if handle:
            kernel.ReleaseMutex(handle)
            kernel.CloseHandle(handle)


def load(path) -> dict[str, str]:
    """Each key's value, in file order. A key that is there twice: the last one counts (as in the game)."""
    settings = {}
    for line in _retry(lambda: _lines(Path(path))):
        entry = _entry(line)
        if entry is not None:
            settings[entry[0]] = entry[1]
    return settings


def update(path, changes: dict[str, str], fallback: dict[str, str] | None = None) -> dict[str, str]:
    """Put only `changes` over what the file says now, and return what it says afterwards. The game and the player's editor
    write the same file, so values read earlier are never written back. `fallback` is used when there is no file, or an empty one."""
    with lock():
        current = load(path) or dict(fallback or {})
        save(path, {**current, **changes})
        return load(path)


def _merge(lines: list[str], values: dict[str, str]) -> list[tuple[str | None, str]]:
    merged = []
    seen = set()
    last = dict(filter(None, map(_entry, lines)))
    for line in lines:
        entry = _entry(line)
        if entry is None:
            merged.append((None, line))
            continue
        key, previous = entry
        if key in seen:
            continue
        seen.add(key)
        value = values.get(key, last[key])
        merged.append((key, line if value == previous else f"{key} = {value}"))

    for key, value in values.items():
        if key not in seen and (key not in _DEFAULTS or value != _DEFAULTS[key]):
            merged.append((key, f"{key} = {value}"))
    return merged


def _tidy(entries: list[tuple[str | None, str]]) -> list[str]:
    header = []
    for key, line in entries:
        if key is not None or not line.startswith("#"):
            break
        header.append(line)
    body = entries[len(header):]
    settings = {key: line for key, line in body if key is not None}

    result = header
    for title, keys in _GROUPS:
        block = [settings.pop(key) for key in keys if key in settings]
        if title == "Controls":
            block.extend(settings.pop(key) for key, _ in body
                         if key is not None and key.startswith(("key_", "pad_")))
        if block:
            result.extend(("", f"# -- {title} --", *block))
    remaining = [line for key, line in body if key is None or key in settings]
    if remaining:
        result.extend(("", "# -- More --", *remaining))
    return result


def save(path, values: dict[str, str]) -> None:
    """Merge edits, preserving existing keys and omitting new default entries."""
    path = Path(path)
    for key, value in values.items():
        if (not key or key != key.strip(_WHITESPACE) or key.startswith(("#", "\ufeff"))
                or any(c in key for c in "=\r\n\0")):
            raise ValueError(f"Invalid INI key: {key!r}")
        if any(c in value for c in "\r\n\0"):
            raise ValueError(f"INI value for {key!r} must fit on a single line.")

    lines = _tidy(_merge(_lines(path), values))
    contents = "".join(line + "\r\n" for line in lines)
    temporary_path = None
    try:
        with NamedTemporaryFile(mode="wb", dir=path.parent, prefix=path.name + ".",
                                suffix=".tmp", delete=False) as temporary:
            temporary_path = Path(temporary.name)
            temporary.write(contents.encode("utf-8", errors="surrogateescape"))
            temporary.flush()
            os.fsync(temporary.fileno())
        _retry(lambda: os.replace(temporary_path, path))
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def effective(values) -> dict[str, str]:
    return {**_DEFAULTS, **values}


def diff_restart_needed(old, new) -> list[str]:
    return [
        option.key for option in OPTIONS
        if option.restart
        and old.get(option.key, option.default) != new.get(option.key, option.default)
    ]
