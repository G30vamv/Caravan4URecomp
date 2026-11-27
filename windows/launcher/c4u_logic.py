# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# Adapted from the logic of the author's other launcher project (core.py): the launcher's own settings, finding the game's
# program, starting and watching it, the update flow. Nothing of the game itself.
"""What the Caravan4URecomp launcher does, without any widget.

The launcher sits next to Caravan4URecomp.exe. It edits `Caravan4URecomp.ini` (the same file the game reads and that the in-game
menu writes), starts the game, and updates the files of the package. The old MH3U logic (core.py) stays in the folder, unused.
"""

import json
import os
import subprocess
import sys
import threading
import time

import c4u_ini

APP_NAME = "Caravan4URecomp"
GAME_EXE = "Caravan4URecomp.exe"
INI_NAME = "Caravan4URecomp.ini"
LOG_NAME = "Caravan4URecomp.log"
CONFIG_NAME = "launcher_config.json"
DEFAULT_VERSION = "0.0.0"


def get_base_dir():
    """The folder of the package: where the launcher's exe is (frozen), or the folder that holds the game's exe (a checkout)."""
    env = os.environ.get("C4U_HOME")
    if env and os.path.isdir(env):
        return os.path.abspath(env)
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    here = os.path.dirname(os.path.abspath(__file__))
    for up in (here, os.path.dirname(here), os.path.dirname(os.path.dirname(here)), os.path.dirname(os.path.dirname(os.path.dirname(here)))):
        for cand in (up, os.path.join(up, "dist", "mh4u-win")):
            if os.path.isfile(os.path.join(cand, GAME_EXE)):
                return cand
    return here


def get_assets_dir():
    if getattr(sys, "frozen", False):
        return os.path.join(getattr(sys, "_MEIPASS", os.path.dirname(sys.executable)), "assets")
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")


def read_version(base_dir):
    """The version of the package: `VERSION.txt` ("Caravan4URecomp 0.3.1") that the packaging script writes."""
    try:
        with open(os.path.join(base_dir, "VERSION.txt"), encoding="utf-8-sig") as f:
            text = f.read().strip().split()
        return text[-1] if text else DEFAULT_VERSION
    except OSError:
        return DEFAULT_VERSION


class C4ULogic:
    """State and actions of the launcher. The window (c4u_launcher_qt.py) calls these and shows the results."""

    def __init__(self):
        self.base_dir = get_base_dir()
        self.version = read_version(self.base_dir)
        self.config_path = os.path.join(self.base_dir, CONFIG_NAME)
        self.ini_path = os.path.join(self.base_dir, INI_NAME)
        self.game_exe = os.path.join(self.base_dir, GAME_EXE)
        self.log_path = os.path.join(self.base_dir, LOG_NAME)
        self.config = self.load_config()
        self.game_process = None
        self._lock = threading.Lock()

    # ---- the launcher's own settings (launcher_config.json) ----------------------------------------------------------
    def load_config(self):
        try:
            with open(self.config_path, encoding="utf-8") as f:
                data = json.load(f)
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def save_config(self):
        tmp = self.config_path + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.config, f, indent=2, ensure_ascii=False)
            os.replace(tmp, self.config_path)
        except OSError:
            pass

    def language(self):
        """'es' or 'en': the launcher's language (config `ui_language`: auto | es | en; auto follows Windows)."""
        choice = self.config.get("ui_language", "auto")
        if choice in ("es", "en"):
            return choice
        try:
            import ctypes

            buf = ctypes.create_unicode_buffer(85)
            ctypes.windll.kernel32.GetUserDefaultLocaleName(buf, 85)
            return "es" if buf.value.lower().startswith("es") else "en"
        except Exception:
            return "en"

    # ---- the game -----------------------------------------------------------------------------------------------------
    def game_exe_present(self):
        return os.path.isfile(self.game_exe)

    def game_state(self, ini_values=None):
        """'missing' (no program next to the launcher), 'needs_setup' (the game's files are not prepared yet) or 'ready'."""
        if not self.game_exe_present():
            return "missing"
        values = ini_values if ini_values is not None else {}
        code = values.get("code", "")
        game = values.get("game", "")
        if code and game and os.path.isfile(code) and os.path.isdir(game):
            return "ready"
        return "needs_setup"

    def is_running(self):
        p = self.game_process
        return p is not None and p.poll() is None

    # ---- the code library: is it the one these tools would make? ---------------------------------------------------------
    def _read_ini_key(self, key):
        try:
            return c4u_ini.load(self.ini_path).get(key, "")
        except OSError:
            return ""

    @staticmethod
    def library_embedded_id(dll):
        """The 16 hex digits of the tools that made the library (the runtime logs them as `tools ...`): after the game version's key
        near the end of the file. None when not found."""
        import re

        try:
            size = os.path.getsize(dll)
            with open(dll, "rb") as f:
                f.seek(max(0, size - 24 * 1024 * 1024))
                data = f.read()
        except OSError:
            return None
        m = re.search(rb"[a-z]{3}_v\d+\x00+([0-9a-f]{16})\x00", data)
        return m.group(1).decode() if m else None

    def library_expected_id(self):
        """The id a library made with the tools of this package has (game-tools: the translator, the runtime and the hook tables),
        by asking those same tools. None when they are not there or do not answer."""
        tools = os.path.join(self.base_dir, "game-tools")
        python = os.path.join(tools, "python", "python.exe")
        if not os.path.isfile(os.path.join(tools, "tools", "build_game.py")) or not os.path.isfile(python):
            return None
        code = "import sys; sys.path.insert(0, 'tools'); import build_game; print(build_game.code_library_id())"
        try:
            out = subprocess.run([python, "-I", "-c", code], cwd=tools, capture_output=True, text=True, timeout=60,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout.strip().splitlines()
        except (OSError, subprocess.SubprocessError):
            return None
        last = out[-1].strip() if out else ""
        return last if len(last) == 16 and all(c in "0123456789abcdef" for c in last) else None

    def library_status(self):
        """'ok', 'stale' (made by other tools than this package's: its hooks or tables changed, so the code library has to be made again),
        'missing' (no library) or 'unknown' (cannot tell: nothing is asked of the user)."""
        dll = self._read_ini_key("code")
        if not dll or not os.path.isfile(dll):
            return "missing"
        have, want = self.library_embedded_id(dll), self.library_expected_id()
        if not have or not want:
            return "unknown"
        return "ok" if have == want else "stale"

    def setup_files(self):
        """(base, update): the game files the library was made from, as the game folder's source.txt records them; '' when unknown or moved."""
        game = self._read_ini_key("game")
        out = {"base": "", "update": ""}
        try:
            with open(os.path.join(game, "source.txt"), encoding="utf-8") as f:
                for line in f:
                    key, _, value = line.strip().partition("=")
                    if key in out:
                        out[key] = value if os.path.isfile(value) else ""
        except OSError:
            pass
        return out["base"], out["update"]

    def recompile(self, on_exit=None):
        """Makes the code library again: starts the program's own window for it (the one of the first start) with the same game files, no
        questions. Returns None or a text with what went wrong."""
        base, update = self.setup_files()
        if not base or not update:
            return "files"
        return self.launch_game(["--setup", "--base-game", base, "--update", update], on_exit=on_exit)

    def ensure_monster_names(self):
        """The life bar's monster names come from the user's own game: monster_names.txt in the game folder, made once by
        game-tools/tools/monster_names.py (a second or two). Never raises: the bar works without the names."""
        try:
            game = ""
            with open(self.ini_path, encoding="utf-8-sig") as f:
                for line in f:
                    key, _, value = line.partition("=")
                    if key.strip() == "game":
                        game = value.strip()
            script = os.path.join(self.base_dir, "game-tools", "tools", "monster_names.py")
            if not game or not os.path.isdir(game) or os.path.isfile(os.path.join(game, "monster_names.txt")) or not os.path.isfile(script):
                return
            import runpy

            old_argv = sys.argv
            try:
                sys.argv = [script, game]
                runpy.run_path(script, run_name="__main__")
            except SystemExit:
                pass
            finally:
                sys.argv = old_argv
        except Exception:
            pass

    def launch_game(self, extra_args=(), on_exit=None):
        """Starts Caravan4URecomp.exe in its folder. Returns None, or a text with what went wrong."""
        if not self.game_exe_present():
            return "missing"
        if self.is_running():
            return "running"
        self.ensure_monster_names()
        flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        try:
            self.game_process = subprocess.Popen([self.game_exe, *extra_args], cwd=self.base_dir, creationflags=flags, close_fds=True)
        except OSError as e:
            return str(e)
        if on_exit:
            proc = self.game_process

            def watch():
                code = proc.wait()
                on_exit(code)

            threading.Thread(target=watch, daemon=True).start()
        return None

    def stop_game(self):
        p = self.game_process
        if p is not None and p.poll() is None:
            try:
                p.terminate()
            except OSError:
                pass

    # ---- folders and the log -----------------------------------------------------------------------------------------
    def hdpack_exe(self):
        """The HD texture converter (mh4n_hdpack.exe), or "" when this package does not have it."""
        for rel in (os.path.join("game-tools", "bin", "mh4n_hdpack.exe"), "mh4n_hdpack.exe"):
            path = os.path.join(self.base_dir, rel)
            if os.path.isfile(path):
                return path
        return ""

    def user_dir(self, ini_values):
        path = ini_values.get("user", "")
        return path if path and os.path.isdir(path) else os.path.join(self.base_dir, "game", "user")

    def save_dirs(self, ini_values):
        """(the game's save data folder, its extra data folder (cards, quests, buddies)): inside <user>\\sdmc, found by the game's id.
        Falls back to <user>\\sdmc when the game has not saved yet."""
        user = self.user_dir(ini_values)
        data = ext = None
        root = os.path.join(user, "sdmc", "Nintendo 3DS")
        try:
            for id0 in os.scandir(root):
                if not id0.is_dir():
                    continue
                for id1 in os.scandir(id0.path):
                    cand = os.path.join(id1.path, "title", "00040000", "00126300", "data")
                    if os.path.isdir(cand):
                        data = data or cand
                    ext_cand = os.path.join(id1.path, "extdata", "00000000", "00001263")
                    if os.path.isdir(ext_cand):
                        ext = ext or ext_cand
        except OSError:
            pass
        fallback = os.path.join(user, "sdmc")
        return (data or fallback, ext or data or fallback)

    def open_folder(self, path):
        try:
            if os.path.isdir(path):
                os.startfile(path)  # noqa: S606 - Windows only, like the whole launcher
        except (OSError, AttributeError):
            pass

    def tail_log(self, lines=60):
        """The last lines of the game's log, or ''."""
        try:
            with open(self.log_path, "rb") as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - 24000))
                data = f.read().decode("utf-8", errors="replace")
            return "\n".join(data.splitlines()[-lines:])
        except OSError:
            return ""

    def saves_backups(self, ini_values):
        """[(name, mtime)] of the automatic backups of the saves, newest first."""
        root = os.path.join(self.user_dir(ini_values), "backups")
        out = []
        try:
            for e in os.scandir(root):
                if e.is_dir() and not e.name.startswith("."):  # (".partial-...": a copy not finished)
                    out.append((e.name, e.stat().st_mtime))
        except OSError:
            pass
        out.sort(key=lambda x: x[1], reverse=True)
        return out

    def wait_until(self, predicate, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if predicate():
                return True
            time.sleep(0.1)
        return False
