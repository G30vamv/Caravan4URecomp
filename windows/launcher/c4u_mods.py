# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# Our own code, written from: windows/mod_manager.h and mh4n/native/src/mods/mod_list.h (the same rules, in Python, so that the
# launcher and the in-game Mods page read and write the same <user>\load\mods\ and load_order.txt).
"""The mods of the port: folders in <user>\\load\\mods\\ (LayeredFS: romfs/, romfs_ext/; code mods: exefs/code.*), their order and
on/off in load_order.txt, and putting a folder or a .zip in place."""

import os
import shutil
import subprocess

PROGRAM_ID = "0004000000126300"
ORDER_FILE = "load_order.txt"
HEADER = "# Written by the launcher's Mods page: one mod a line, first loaded first; '# off: Name' = turned off (keeps its place).\n"


def mods_dir(user_dir):
    return os.path.join(user_dir, "load", "mods")


def _is_hex16(name):
    return len(name) == 16 and all(c in "0123456789abcdefABCDEF" for c in name)


def looks_like_mod(path):
    for sub in ("romfs", "romfs_ext", "exefs"):
        if os.path.isdir(os.path.join(path, sub)):
            return True
    return any(os.path.isfile(os.path.join(path, f)) for f in ("exheader.bin", "code.ips", "code.bps", "code.bin"))


def _info(path):
    """(has_romfs, is_code_mod) of a mod folder."""
    romfs = os.path.isdir(os.path.join(path, "romfs")) or os.path.isdir(os.path.join(path, "romfs_ext"))
    code = os.path.isfile(os.path.join(path, "exheader.bin"))
    for base in (path, os.path.join(path, "exefs")):
        for f in ("code.ips", "code.bps", "code.bin"):
            if os.path.isfile(os.path.join(base, f)):
                code = True
    return romfs, code


def _read_order(folder):
    """[(name, on)] in the file's order."""
    out = []
    try:
        with open(os.path.join(folder, ORDER_FILE), encoding="utf-8-sig") as f:
            for raw in f:
                line = raw.strip()
                if not line:
                    continue
                if line.lower().startswith("# off:"):
                    out.append((line[6:].strip(), False))
                elif line.startswith("#"):
                    continue
                else:
                    out.append((line, True))
    except OSError:
        pass
    return out


def list_mods(user_dir):
    """[{name, on, romfs, code}] in load order, as the game reads them."""
    folder = mods_dir(user_dir)
    try:
        names = sorted(e.name for e in os.scandir(folder) if e.is_dir() and not e.name.startswith((".", "_")))
    except OSError:
        return []
    names = [n for n in names if not (_is_hex16(n) and n != PROGRAM_ID)]
    state = {}
    order = []
    for name, on in _read_order(folder):
        if name in names and name not in state:
            state[name] = on
            order.append(name)
    for n in names:
        if n not in state:
            state[n] = True
            order.append(n)
    result = []
    for n in order:
        romfs, code = _info(os.path.join(folder, n))
        result.append({"name": n, "on": state[n], "romfs": romfs, "code": code})
    return result


def save_order(user_dir, mods):
    folder = mods_dir(user_dir)
    os.makedirs(folder, exist_ok=True)
    lines = [HEADER]
    for m in mods:
        lines.append(("" if m["on"] else "# off: ") + m["name"] + "\n")
    tmp = os.path.join(folder, ORDER_FILE + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write("".join(lines))
    os.replace(tmp, os.path.join(folder, ORDER_FILE))


def set_on(user_dir, name, on):
    mods = list_mods(user_dir)
    for m in mods:
        if m["name"] == name:
            m["on"] = bool(on)
            save_order(user_dir, mods)
            return True
    return False


def move(user_dir, name, by):
    mods = list_mods(user_dir)
    idx = next((i for i, m in enumerate(mods) if m["name"] == name), -1)
    if idx < 0:
        return False
    to = max(0, min(len(mods) - 1, idx + by))
    if to != idx:
        mods[idx], mods[to] = mods[to], mods[idx]
        save_order(user_dir, mods)
    return True


def _find_root(path, depth=3):
    if looks_like_mod(path):
        return path
    if depth <= 0:
        return None
    try:
        subs = [e.path for e in os.scandir(path) if e.is_dir()]
    except OSError:
        return None
    if len(subs) == 1:
        return _find_root(subs[0], depth - 1)
    game = None
    for sub in subs:  # emulator-style packages: one folder per title id; the update's (0004000E...) is the one that applies
        n = os.path.basename(sub)
        if _is_hex16(n) and looks_like_mod(sub):
            if n.upper().startswith("0004000E"):
                return sub
            game = sub
    return game


def safe_name(name):
    for c in '<>:"/\\|?*':
        name = name.replace(c, "_")
    name = "".join("_" if ord(c) < 32 else c for c in name)
    name = name.rstrip(" .").lstrip("._ ")
    return name


def install(user_dir, dropped):
    """Puts a mod folder or .zip in <user>\\load\\mods\\. Returns (ok, name_or_message, turned_off_because_code)."""
    folder = mods_dir(user_dir)
    os.makedirs(folder, exist_ok=True)
    source = dropped
    temp = None
    name = os.path.splitext(os.path.basename(dropped))[0]
    if os.path.isfile(dropped):
        if not dropped.lower().endswith(".zip"):
            return False, "not_zip", False
        temp = os.path.join(folder, ".installing")
        shutil.rmtree(temp, ignore_errors=True)
        os.makedirs(temp)
        tar = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            ok = subprocess.run([tar, "-xf", dropped, "-C", temp], creationflags=flags, timeout=180).returncode == 0
        except (OSError, subprocess.SubprocessError):
            ok = False
        if not ok:
            shutil.rmtree(temp, ignore_errors=True)
            return False, "unzip_failed", False
        source = temp
    elif os.path.isdir(dropped):
        name = os.path.basename(os.path.normpath(dropped))
    else:
        return False, "not_found", False
    root = _find_root(source)
    if not root:
        if temp:
            shutil.rmtree(temp, ignore_errors=True)
        return False, "not_a_mod", False
    name = safe_name(name) or "mod"
    unique = name
    n = 2
    while os.path.exists(os.path.join(folder, unique)) and n < 100:
        unique = f"{name} ({n})"
        n += 1
    try:
        shutil.copytree(root, os.path.join(folder, unique))
    except OSError as e:
        return False, f"copy_failed: {e}", False
    finally:
        if temp:
            shutil.rmtree(temp, ignore_errors=True)
    _romfs, code = _info(os.path.join(folder, unique))
    if code:
        set_on(user_dir, unique, False)
    return True, unique, code


def remove(user_dir, name):
    """Deletes the mod's folder and its place in the list."""
    path = os.path.join(mods_dir(user_dir), name)
    if os.path.isdir(path) and not name.startswith((".", "_")):
        shutil.rmtree(path, ignore_errors=True)
        mods = [m for m in list_mods(user_dir) if m["name"] != name]
        save_order(user_dir, mods)
        return True
    return False
