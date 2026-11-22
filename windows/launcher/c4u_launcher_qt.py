# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# The window is the one of the author's other launcher project (mh3u_launcher_qt.py) in this project's palette; the pages are new.
"""Caravan4URecomp launcher: edits Caravan4URecomp.ini (every option of the in-game menu, and the ones that only apply when the game
starts), starts the game and updates the files of the package. What it does lives in c4u_logic.py; the pages of options are made by
c4u_form.py from the schema (c4u_options.py)."""

import os
import queue
import sys
import threading
import webbrowser

from PySide6.QtCore import QEvent, QTimer, Qt
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QApplication, QGridLayout, QHBoxLayout, QLabel, QScrollArea, QVBoxLayout, QWidget

import qtanim
import qtglass
import qtmenu
import qtshim
import qttitle
import winfx
from c4u_form import OptionsForm
from c4u_logic import APP_NAME, C4ULogic, get_assets_dir
from qtglass import BackdropWidget, GlassPanel
from qtshim import Button, Label, LogBox, ProgressBar, Switch, TabView

try:  # the two modules of the work packages LA01 / LA02
    import c4u_ini
    import c4u_mods
    import c4u_options
except Exception as e:  # pragma: no cover - a launcher without them cannot edit the ini
    c4u_ini = c4u_options = None
    print(f"launcher: options module missing: {e}")
try:
    import c4u_updater
except Exception:  # pragma: no cover
    c4u_updater = None

REPO_URL = "https://github.com/G30vamv/Caravan4URecomp"

CREAM = "#e3d8bd"
MUTED = "#c7b48f"
GOLD_TEXT = "#eab84c"
BLUE = "#35c6dd"

STYLE = """
* { font-family: "Segoe UI"; }
QToolTip { background: #1a1208; color: #e3d8bd; border: 1px solid #5a4128; padding: 4px; }
QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: rgba(200,150,80,110); border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: rgba(212,146,42,200); }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QLineEdit { background: rgba(13,8,4,185); color: #fff6e0; border: 1px solid rgba(255,255,255,45);
            border-radius: 9px; padding: 4px 10px; selection-background-color: #1f9db8; }
QLineEdit:focus { border: 1px solid #d4922a; }
QComboBox { background: rgba(13,8,4,185); color: #fff6e0; border: 1px solid rgba(255,255,255,45);
            border-radius: 9px; padding: 4px 34px 4px 10px; }
QComboBox:hover { border: 1px solid #d4922a; }
QComboBox::drop-down { border: none; width: 0; }
QComboBox QAbstractItemView { background: #1a1208; color: #e3d8bd; border: 1px solid #5a4128;
            selection-background-color: #b8791e; selection-color: #fff6e0; outline: none; }
QSlider::groove:horizontal { height: 6px; background: rgba(13,8,4,200); border-radius: 3px; }
QSlider::sub-page:horizontal { background: #1f9db8; border-radius: 3px; }
QSlider::handle:horizontal { background: #eab84c; width: 16px; margin: -6px 0; border-radius: 8px; }
QSlider::handle:horizontal:hover { background: #f2c968; }
QPlainTextEdit#log { background: rgba(10,6,3,205); color: #f0e4c0; border: 1px solid rgba(255,255,255,40);
            border-radius: 10px; padding: 6px; selection-background-color: #1f9db8; }
QMessageBox, QDialog { background: #26170a; }
QMessageBox QLabel { color: #e3d8bd; }
QMessageBox QPushButton { background: #3a2a18; color: #fff6e0; border: 1px solid rgba(255,255,255,40);
            border-radius: 8px; padding: 6px 18px; min-width: 70px; }
QMessageBox QPushButton:hover { background: #b8791e; }
"""

# the pages (tab key of the schema -> English, Spanish); "play" is the first one and has no options
PAGES = [
    ("play", "Play", "Jugar"),
    ("graphics", "Graphics", "Gráficos"),
    ("game", "Game", "Juego"),
    ("controls", "Controls", "Controles"),
    ("audio", "Audio", "Audio"),
    ("online", "Online", "En línea"),
    ("saves", "Saves", "Partidas"),
    ("mods", "Mods", "Mods"),
    ("system", "System", "Sistema"),
]


class Window(QWidget):
    def __init__(self, app):
        super().__init__()
        self._app = app
        self.setWindowFlags(
            Qt.WindowType.Window | Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowMinMaxButtonsHint | Qt.WindowType.WindowSystemMenuHint
        )
        qttitle.install_grips(self)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        qttitle.place_grips(self)

    def changeEvent(self, event):
        super().changeEvent(event)
        if event.type() == QEvent.Type.WindowStateChange and bool(event.oldState() & Qt.WindowState.WindowMinimized) and not self.isMinimized():
            qtanim.fade_window_in(self, 170)
        if hasattr(self._app, "titlebar"):
            self._app.titlebar.sync()
            qttitle.place_grips(self)

    def closeEvent(self, event):
        self._app.on_app_close()
        event.accept()


class IniStore:
    """The values of the ini: what the file says plus the game's defaults. Every `set` is written at once."""

    def __init__(self, path):
        self.path = path
        self.values = c4u_ini.load(path) if c4u_ini else {}

    def reload(self):
        self.values = c4u_ini.load(self.path) if c4u_ini else {}

    def get(self, key):
        if key in self.values:
            return self.values[key]
        try:
            return c4u_options.by_key(key).default
        except Exception:
            return ""

    def set(self, key, value):
        self.values[key] = value
        if c4u_ini:
            c4u_ini.save(self.path, self.values)


class C4ULauncherApp(C4ULogic):
    def __init__(self, qapp):
        super().__init__()
        self.qapp = qapp
        self.assets_dir = get_assets_dir()
        self.store = IniStore(self.ini_path)
        self.ui_queue = queue.Queue()
        qtanim.set_user_choice(self.config.get("animations", True))
        self.update_info = None
        self._busy_update = False

        self.window = Window(self)
        self.window.setWindowTitle(f"{APP_NAME} {self.version}")
        icon_path = os.path.join(self.assets_dir, "caravan4u.ico")
        if os.path.exists(icon_path):
            self.window.setWindowIcon(QIcon(icon_path))
        self.window.resize(1120, 800)
        self.window.setMinimumSize(940, 660)
        qtshim.center_on_primary(self.window)
        qtshim.set_main_window(self.window)

        self.acrylic = bool(self.config.get("acrylic", False)) and winfx.acrylic_available()
        if self.acrylic:
            self.window.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.backdrop = BackdropWidget(None, self.window, opacity=0.50 if self.acrylic else 1.0)
        qtglass.SEE_THROUGH = self.acrylic
        outer = QVBoxLayout(self.window)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(self.backdrop)
        self.form = OptionsForm(c4u_options, self.store, self.language, self._option_changed) if c4u_options else None
        self.build_ui()

        self.poll = QTimer()
        self.poll.timeout.connect(self._process_ui_queue)
        self.poll.start(60)
        self.log_timer = QTimer()
        self.log_timer.timeout.connect(self.refresh_log)
        self.log_timer.start(1500)
        self.refresh_status()
        if c4u_updater:
            threading.Thread(target=self._prune, daemon=True).start()
            if self.config.get("auto_check_updates", True):
                QTimer.singleShot(3500, lambda: self.check_for_updates(silent=True))

    # ---- helpers -----------------------------------------------------------------------------------------------------------
    def tr(self, en, es):
        return es if self.language() == "es" else en

    def show(self):
        screen_index = os.environ.get("C4U_SCREEN")  # tests: open on another screen than the main one
        if screen_index and screen_index.isdigit() and int(screen_index) < len(self.qapp.screens()):
            g = self.qapp.screens()[int(screen_index)].geometry()
            self.window.move(g.x() + 80, g.y() + 60)
        self.window.show()
        winfx.style_window(self.window.winId(), self.acrylic)

    def after(self, ms, fn):
        if threading.current_thread() is threading.main_thread():
            QTimer.singleShot(ms, fn)
        else:
            self.ui_queue.put(("call", (ms, fn)))

    def _prune(self):
        try:
            c4u_updater.prune_backups(self.base_dir)
        except Exception:
            pass

    def _process_ui_queue(self):
        try:
            while not self.ui_queue.empty():
                kind, data = self.ui_queue.get_nowait()
                if kind == "call":
                    ms, fn = data
                    QTimer.singleShot(ms, fn)
                elif kind == "update_state":
                    self.lbl_update_status.setText(data)
                elif kind == "update_available":
                    self._show_update(data)
                elif kind == "progress":
                    self.progress.set(data)
                elif kind == "game_exit":
                    self._game_exited(data)
                elif kind == "info":
                    qtshim.messagebox.showinfo(*data)
                elif kind == "error":
                    qtshim.messagebox.showerror(*data)
        except Exception as e:  # pragma: no cover
            print(f"UI queue error: {e}")

    # ---- window ------------------------------------------------------------------------------------------------------------
    def build_ui(self):
        root = QVBoxLayout(self.backdrop)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        icon_path = os.path.join(self.assets_dir, "caravan4u.ico")
        self.titlebar = qttitle.TitleBar(self.window, f"{APP_NAME} {self.version}", icon_path if os.path.exists(icon_path) else None)
        root.addWidget(self.titlebar)
        self._build_menu()

        content = QWidget()
        content_lay = QVBoxLayout(content)
        content_lay.setContentsMargins(20, 2, 20, 12)
        content_lay.setSpacing(12)
        root.addWidget(content, 1)
        self.header = self.create_header()
        content_lay.addWidget(self.header)

        self.tabview = TabView()
        content_lay.addWidget(self.tabview, 1)
        self.tabview.add(self.tr("Play", "Jugar"), self.setup_tab_play())
        self._tab_names = {"play": self.tr("Play", "Jugar").strip()}
        for key, en, es in PAGES[1:]:
            if key == "saves":
                page = self.setup_tab_saves()
            elif key == "mods":
                page = self.setup_tab_mods()
            elif self.form is not None and self._has_options(key):
                page = self.form.build_page(key)
            else:
                continue
            name = self.tr(en, es)
            self.tabview.add(name, page)
            self._tab_names[key] = name.strip()
        self.tabview.animate = True
        self.footer = self.create_footer()
        content_lay.addWidget(self.footer)

    def _has_options(self, tab):
        try:
            return any(o.visible for o in c4u_options.by_tab(tab))
        except Exception:
            return False

    def _build_menu(self):
        self.menu = qtmenu.SettingsMenu(self.window)
        gear = self.titlebar.btn_menu

        def closed():
            gear.menu_open = False
            gear.update()

        self.menu.on_hidden = closed
        self.menu.add_title("IDIOMA / LANGUAGE")
        self.seg_language = self.menu.add_choice(
            ["Auto", "Español", "English"],
            {"es": "Español", "en": "English"}.get(self.config.get("ui_language", "auto"), "Auto"),
            self.on_language_changed,
        )
        self.menu.add_separator()
        self.menu.add_title(self.tr("LAUNCHER OPTIONS", "OPCIONES DEL LAUNCHER"))
        self.sw_animations = self.menu.add_switch(
            self.tr("💡 Animations", "💡 Animaciones"),
            bool(self.config.get("animations", True)),
            self.on_animations_toggled,
            qtanim.tip_html(self.tr("Soft changes of page and buttons. They turn off by themselves if Windows has animations off.",
                                    "Suaviza los cambios de pestaña y los botones. Se desactivan solas si Windows tiene las animaciones apagadas.")),
        )
        self.sw_acrylic = self.menu.add_switch(
            self.tr("💡 Acrylic window effect", "💡 Efecto Acrylic de la ventana"),
            bool(self.config.get("acrylic", False)),
            self.on_acrylic_toggled,
            qtanim.tip_html(self.tr("The desktop shows through the window. Windows 11 only. Restart the launcher to see the change.",
                                    "El escritorio se transparenta detrás de la ventana. Solo en Windows 11. Reinicia el launcher para ver el cambio.")),
        )
        self.sw_auto_updates = self.menu.add_switch(
            self.tr("💡 Check for updates on start", "💡 Buscar actualizaciones al abrir"),
            bool(self.config.get("auto_check_updates", True)),
            self.on_auto_updates_toggled,
            qtanim.tip_html(self.tr("Looks for a new version when the launcher opens and tells you. It installs nothing without asking.",
                                    "Al abrir comprueba si hay una versión nueva y te avisa. No instala nada sin preguntarte.")),
        )
        self.sw_minimize = self.menu.add_switch(
            self.tr("💡 Minimize while playing", "💡 Minimizar al jugar"),
            bool(self.config.get("minimize_on_play", True)),
            self.on_minimize_toggled,
            qtanim.tip_html(self.tr("The launcher goes to the taskbar when the game starts and comes back when it closes.",
                                    "El launcher se va a la barra de tareas al abrir el juego y vuelve al cerrarlo.")),
        )
        self.menu.add_separator()
        self.menu.add_action(self.tr("🔄  Check for updates now", "🔄  Buscar actualizaciones ahora"), lambda: self.check_for_updates(False))
        self.menu.add_action(self.tr("🌐  Project page", "🌐  Página del proyecto"), lambda: webbrowser.open(REPO_URL))
        gear.clicked.connect(self.open_menu)

    def open_menu(self):
        if self.menu.isVisible():
            self.menu.close()
            return
        if qtanim.popup_just_closed():
            return
        self.titlebar.btn_menu.menu_open = True
        self.titlebar.btn_menu.update()
        self.menu.popup_below(self.titlebar.btn_menu)

    def on_language_changed(self, value):
        self.config["ui_language"] = {"Español": "es", "English": "en"}.get(value, "auto")
        self.save_config()
        qtshim.messagebox.showinfo(
            "Caravan4URecomp",
            "Language changed. It applies the next time you open the launcher.\n\nIdioma cambiado. Se aplica la próxima vez que abras el launcher.",
        )

    def on_animations_toggled(self):
        on = bool(self.sw_animations.get())
        self.config["animations"] = on
        self.save_config()
        qtanim.set_user_choice(on)
        glow = getattr(self, "_glow", None)
        if glow:
            glow.refresh()

    def on_acrylic_toggled(self):
        self.config["acrylic"] = bool(self.sw_acrylic.get())
        self.save_config()
        qtshim.messagebox.showinfo(self.tr("Acrylic effect", "Efecto Acrylic"),
                                   self.tr("The change applies the next time you open the launcher.", "El cambio se aplicará la próxima vez que abras el launcher."))

    def on_auto_updates_toggled(self):
        self.config["auto_check_updates"] = bool(self.sw_auto_updates.get())
        self.save_config()

    def on_minimize_toggled(self):
        self.config["minimize_on_play"] = bool(self.sw_minimize.get())
        self.save_config()

    def create_header(self):
        header = GlassPanel("card", 20)
        lay = QHBoxLayout(header)
        lay.setContentsMargins(20, 14, 18, 14)
        lay.setSpacing(16)
        logo = QLabel()
        icon = os.path.join(self.assets_dir, "caravan4u.ico")
        if os.path.exists(icon):
            logo.setPixmap(QIcon(icon).pixmap(64, 64))
        lay.addWidget(logo)
        text = QVBoxLayout()
        text.setSpacing(2)
        text.addWidget(Label("CARAVAN4URECOMP", "#f2b94b", 24, bold=True, family="Georgia"))
        text.addWidget(Label(self.tr("Monster Hunter 4 Ultimate for Windows • native, no emulator",
                                     "Monster Hunter 4 Ultimate para Windows • nativo, sin emulador"), "#e2d2a6", 11))
        lay.addLayout(text, 1)

        badge = GlassPanel("inset", 14)
        blay = QVBoxLayout(badge)
        blay.setContentsMargins(14, 8, 14, 8)
        blay.setSpacing(4)
        blay.addWidget(Label(self.tr(f"Version {self.version}", f"Versión {self.version}"), GOLD_TEXT, 10, bold=True), 0, Qt.AlignmentFlag.AlignHCenter)
        self.btn_update = Button(self.tr("⟳  Check for updates", "⟳  Buscar actualizaciones"), "teal", self.on_update_button, width=210, height=30, size=10)
        self.lbl_update_status = Label("", MUTED, 9.5)
        self.lbl_update_status.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.lbl_update_status.setWordWrap(True)
        self.lbl_update_status.setFixedWidth(210)
        self.progress = ProgressBar(8)
        self.progress.setVisible(False)
        blay.addWidget(self.btn_update)
        blay.addWidget(self.lbl_update_status)
        blay.addWidget(self.progress)
        lay.addWidget(badge)
        return header

    def create_footer(self):
        footer = QWidget()
        lay = QHBoxLayout(footer)
        lay.setContentsMargins(6, 0, 6, 0)
        lay.addWidget(Label(self.tr("Modified version of 4u-native by khorne-hub • GPL-3.0 • no game files included",
                                    "Versión modificada de 4u-native de khorne-hub • GPL-3.0 • no incluye archivos del juego"), MUTED, 9.5))
        lay.addStretch(1)
        lay.addWidget(Label("G30vamv and the Caravan4URecomp contributors", CREAM, 9.5, bold=True))
        return footer

    # ---- page: play --------------------------------------------------------------------------------------------------------
    def setup_tab_play(self):
        page = QWidget()
        lay = QVBoxLayout(page)
        lay.setContentsMargins(4, 4, 4, 4)
        lay.setSpacing(12)

        status = GlassPanel("card", 16)
        sg = QGridLayout(status)
        sg.setContentsMargins(18, 14, 18, 14)
        sg.setHorizontalSpacing(12)
        sg.setVerticalSpacing(8)
        sg.addWidget(Label(self.tr("GAME:", "JUEGO:"), None, 11, bold=True), 0, 0)
        self.lbl_state = Label("", GOLD_TEXT, 11, bold=True)
        sg.addWidget(self.lbl_state, 0, 1)
        sg.addWidget(Label(self.tr("SUMMARY:", "RESUMEN:"), None, 10.5, bold=True), 1, 0)
        self.lbl_summary = Label("", CREAM, 10.5, bold=True)
        self.lbl_summary.setWordWrap(True)
        sg.addWidget(self.lbl_summary, 1, 1)
        sg.setColumnStretch(1, 1)
        lay.addWidget(status)

        self.btn_play = Button("", "gold", self.on_play, height=64, size=16)
        self.btn_play.setMinimumWidth(460)
        lay.addWidget(self.btn_play, 0, Qt.AlignmentFlag.AlignHCenter)
        self._glow = qtanim.Glow(self.btn_play, self._glow_allowed)

        row = QHBoxLayout()
        row.setSpacing(12)
        actions = GlassPanel("card", 16)
        al = QVBoxLayout(actions)
        al.setContentsMargins(18, 14, 18, 14)
        al.setSpacing(8)
        al.addWidget(Label(self.tr("Quick actions", "Acciones rápidas"), BLUE, 11.5, bold=True))
        self.btn_setup = Button("", "default", self.on_setup)
        al.addWidget(self.btn_setup)
        al.addWidget(Button(self.tr("📂  Open the game's folder", "📂  Abrir la carpeta del programa"), "default", lambda: self.open_folder(self.base_dir)))
        al.addWidget(Button(self.tr("💾  Open the saves folder", "💾  Abrir la carpeta de partidas"),
                            "default", lambda: self.open_folder(self.save_dirs(self.store.values)[0])))
        al.addWidget(Button(self.tr("📄  Open the HD textures folder", "📄  Abrir la carpeta de texturas HD"), "default", self.open_textures))
        al.addStretch(1)
        row.addWidget(actions, 2)

        logcard = GlassPanel("card", 16)
        ll = QVBoxLayout(logcard)
        ll.setContentsMargins(18, 14, 18, 14)
        ll.setSpacing(8)
        ll.addWidget(Label(self.tr("Game log (last lines)", "Registro del juego (últimas líneas)"), BLUE, 11.5, bold=True))
        self.txt_log = LogBox(170)
        ll.addWidget(self.txt_log, 1)
        row.addWidget(logcard, 3)
        lay.addLayout(row, 1)

        tip = GlassPanel("inset", 14)
        tl = QHBoxLayout(tip)
        tl.setContentsMargins(16, 10, 16, 10)
        tl.addWidget(Label("💡", None, 16))
        text = Label(
            self.tr("Everything you change in the pages above is saved at once into Caravan4URecomp.ini, the same file the in-game menu (F1) uses. "
                    "The options that only apply when the game starts (resolution scale, HD textures, render thread...) can be changed here.",
                    "Todo lo que cambies en las páginas de arriba se guarda al instante en Caravan4URecomp.ini, el mismo archivo que usa el menú del juego (F1). "
                    "Las opciones que solo valen al abrir el juego (escala de resolución, texturas HD, hilo de render...) se pueden cambiar aquí."),
            CREAM, 10)
        text.setWordWrap(True)
        tl.addWidget(text, 1)
        lay.addWidget(tip)
        return page

    def _glow_allowed(self):
        w = self.window
        return (qtanim.enabled() and not self.is_running() and self.btn_play.isEnabled() and w.isVisible() and w.isActiveWindow()
                and not w.isMinimized() and self.tabview.current() == self._tab_names.get("play"))

    # ---- page: saves -------------------------------------------------------------------------------------------------------
    def setup_tab_saves(self):
        page = QWidget()
        lay = QVBoxLayout(page)
        lay.setContentsMargins(4, 4, 10, 4)
        lay.setSpacing(12)
        if self.form is not None and self._has_options("saves"):
            lay.addWidget(self.form.build_page("saves"), 1)
        card = GlassPanel("card", 16)
        cl = QVBoxLayout(card)
        cl.setContentsMargins(18, 14, 18, 14)
        cl.setSpacing(8)
        cl.addWidget(Label(self.tr("YOUR SAVES", "TUS PARTIDAS"), GOLD_TEXT, 10, bold=True))
        self.lbl_backups = Label("", CREAM, 10.5)
        self.lbl_backups.setWordWrap(True)
        cl.addWidget(self.lbl_backups)
        btns = QHBoxLayout()
        btns.addWidget(Button(self.tr("💾  Open the saves", "💾  Abrir las partidas"), "default", lambda: self.open_folder(self.save_dirs(self.store.values)[0])))
        btns.addWidget(Button(self.tr("🃏  Open guild cards and quests", "🃏  Abrir tarjetas y misiones"), "default", lambda: self.open_folder(self.save_dirs(self.store.values)[1])))
        btns.addWidget(Button(self.tr("🗂  Open backups folder", "🗂  Abrir carpeta de copias"), "default",
                              lambda: self.open_folder(os.path.join(self.user_dir(self.store.values), "backups"))))
        btns.addStretch(1)
        cl.addLayout(btns)
        lay.addWidget(card)
        lay.addStretch(1)
        return page

    # ---- page: mods --------------------------------------------------------------------------------------------------------
    def setup_tab_mods(self):
        page = QWidget()
        lay = QVBoxLayout(page)
        lay.setContentsMargins(4, 4, 10, 4)
        lay.setSpacing(12)

        top = GlassPanel("card", 16)
        tl = QVBoxLayout(top)
        tl.setContentsMargins(18, 14, 18, 14)
        tl.setSpacing(8)
        tl.addWidget(Label(self.tr("MODS", "MODS"), GOLD_TEXT, 10, bold=True))
        note = Label(self.tr(
            "A mod is a folder (or a .zip) with romfs/, romfs_ext/ or exefs/. They load in the order shown (the last one wins when two change the "
            "same file) and apply the next time the game starts. A mod that changes the game's code is installed turned off: this port cannot use it.",
            "Un mod es una carpeta (o un .zip) con romfs/, romfs_ext/ o exefs/. Se cargan en el orden de la lista (el último gana si dos cambian el mismo "
            "archivo) y se aplican la próxima vez que abras el juego. Un mod que cambia el código del juego se instala apagado: este port no lo puede usar."),
            MUTED, 10)
        note.setWordWrap(True)
        tl.addWidget(note)
        row = QHBoxLayout()
        row.addWidget(Button(self.tr("➕  Add a .zip", "➕  Añadir un .zip"), "gold", self.on_mod_add_zip, height=34, size=10))
        row.addWidget(Button(self.tr("📁  Add a folder", "📁  Añadir una carpeta"), "default", self.on_mod_add_folder, height=34, size=10))
        row.addWidget(Button(self.tr("📂  Open the mods folder", "📂  Abrir la carpeta de mods"), "default", self.on_mods_open, height=34, size=10))
        row.addStretch(1)
        tl.addLayout(row)
        self.lbl_mods_msg = Label("", GOLD_TEXT, 10)
        self.lbl_mods_msg.setWordWrap(True)
        tl.addWidget(self.lbl_mods_msg)
        lay.addWidget(top)

        self.mods_card = GlassPanel("card", 16)
        self.mods_lay = QVBoxLayout(self.mods_card)
        self.mods_lay.setContentsMargins(18, 14, 18, 14)
        self.mods_lay.setSpacing(8)
        lay.addWidget(self.mods_card)
        lay.addStretch(1)
        self.refresh_mods()
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.viewport().setAutoFillBackground(False)
        page.setAutoFillBackground(False)
        scroll.setWidget(page)
        return scroll

    def refresh_mods(self):
        while self.mods_lay.count():
            item = self.mods_lay.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
            elif item.layout():
                self._clear_layout(item.layout())
        user = self.user_dir(self.store.values)
        mods = c4u_mods.list_mods(user)
        self.mods_lay.addWidget(Label(self.tr("INSTALLED (load order)", "INSTALADOS (orden de carga)"), GOLD_TEXT, 10, bold=True))
        if not mods:
            self.mods_lay.addWidget(Label(self.tr("No mods yet. Add a .zip or a folder.", "Aún no hay mods. Añade un .zip o una carpeta."), MUTED, 10.5))
            return
        for m in mods:
            name = m["name"]
            row = QWidget()
            h = QHBoxLayout(row)
            h.setContentsMargins(0, 0, 0, 0)
            h.setSpacing(8)
            sw = Switch(name, lambda n=name: self.on_mod_toggle(n))
            if m["on"]:
                sw.select()
            h.addWidget(sw, 1)
            tags = []
            if m["romfs"]:
                tags.append("romfs")
            if m["code"]:
                tags.append(self.tr("code (not usable)", "código (no usable)"))
            if tags:
                h.addWidget(Label("  ".join(tags), MUTED, 9.5))
            h.addWidget(Button("▲", "default", lambda n=name: self.on_mod_move(n, -1), width=40, height=30, size=10))
            h.addWidget(Button("▼", "default", lambda n=name: self.on_mod_move(n, 1), width=40, height=30, size=10))
            h.addWidget(Button(self.tr("Remove", "Quitar"), "danger", lambda n=name: self.on_mod_remove(n), width=80, height=30, size=10))
            self.mods_lay.addWidget(row)

    def _clear_layout(self, layout):
        while layout.count():
            it = layout.takeAt(0)
            if it.widget():
                it.widget().deleteLater()

    def _mods_say(self, text):
        self.lbl_mods_msg.setText(text)

    def on_mod_toggle(self, name):
        user = self.user_dir(self.store.values)
        mods = c4u_mods.list_mods(user)
        cur = next((m for m in mods if m["name"] == name), None)
        if cur is None:
            return
        new_on = not cur["on"]
        c4u_mods.set_on(user, name, new_on)
        if new_on and cur["code"]:
            self._mods_say(self.tr(f"{name} changes the game's code: the game turns it off when it starts.",
                                   f"{name} cambia el código del juego: el juego lo apaga al arrancar."))
        else:
            self._mods_say(self.tr("Saved. It applies the next time the game starts.", "Guardado. Se aplica la próxima vez que abras el juego."))

    def on_mod_move(self, name, by):
        c4u_mods.move(self.user_dir(self.store.values), name, by)
        self.refresh_mods()

    def on_mod_remove(self, name):
        if qtshim.messagebox.askyesno("Caravan4URecomp", self.tr(f"Remove the mod {name}? Its folder is deleted.", f"¿Quitar el mod {name}? Se borra su carpeta.")):
            c4u_mods.remove(self.user_dir(self.store.values), name)
            self.refresh_mods()

    def _mod_result(self, ok, value, code):
        if ok:
            if code:
                self._mods_say(self.tr(f"Installed {value}, turned off: it changes the game's code, which this port cannot use.",
                                       f"Instalado {value}, apagado: cambia el código del juego, que este port no puede usar."))
            else:
                self._mods_say(self.tr(f"Installed {value}. It applies the next time the game starts.", f"Instalado {value}. Se aplica la próxima vez que abras el juego."))
        else:
            reasons = {
                "not_zip": self.tr("A mod is a folder or a .zip.", "Un mod es una carpeta o un .zip."),
                "unzip_failed": self.tr("The .zip could not be unpacked.", "No se pudo descomprimir el .zip."),
                "not_a_mod": self.tr("That is not a mod: it has no romfs, romfs_ext or exefs folder.", "Eso no es un mod: no tiene carpeta romfs, romfs_ext ni exefs."),
                "not_found": self.tr("That path does not exist.", "Esa ruta no existe."),
            }
            self._mods_say(reasons.get(value, str(value)))
        self.refresh_mods()

    def on_mod_add_zip(self):
        path = qtshim.filedialog.askopenfilename(self.tr("Choose a mod (.zip)", "Elige un mod (.zip)"), "", "Mods (*.zip)")
        if path:
            self._mod_result(*c4u_mods.install(self.user_dir(self.store.values), path))

    def on_mod_add_folder(self):
        path = qtshim.filedialog.askdirectory(self.tr("Choose the mod's folder", "Elige la carpeta del mod"), "")
        if path:
            self._mod_result(*c4u_mods.install(self.user_dir(self.store.values), path))

    def on_mods_open(self):
        folder = c4u_mods.mods_dir(self.user_dir(self.store.values))
        os.makedirs(folder, exist_ok=True)
        self.open_folder(folder)

    # ---- status ------------------------------------------------------------------------------------------------------------
    def refresh_status(self):
        self.store.reload()
        if self.form is not None:
            self.form.reload_all()
        state = self.game_state(self.store.values)
        running = self.is_running()
        texts = {
            "missing": self.tr("Caravan4URecomp.exe not found next to the launcher", "No se encuentra Caravan4URecomp.exe junto al launcher"),
            "needs_setup": self.tr("Not prepared yet: the first start asks for your game files", "Sin preparar: la primera vez te pide los archivos de tu juego"),
            "ready": self.tr("Ready to play", "Listo para jugar"),
        }
        self.lbl_state.setText(self.tr("Running...", "En ejecución...") if running else texts[state])
        if state == "ready" or state == "needs_setup":
            play = self.tr("⚔️  PLAY", "⚔️  JUGAR") if state == "ready" else self.tr("🛠️  PREPARE THE GAME AND PLAY", "🛠️  PREPARAR EL JUEGO Y JUGAR")
        else:
            play = self.tr("Program not found", "Programa no encontrado")
        self.btn_play.configure(text=play, state="normal" if (state != "missing" and not running) else "disabled")
        self.btn_setup.configure(text=self.tr("🛠️  Prepare the game again (choose files)", "🛠️  Preparar el juego de nuevo (elegir archivos)"),
                                 state="normal" if state != "missing" else "disabled")
        self.lbl_summary.setText(self.summary())
        self.refresh_saves_label()
        self.refresh_log()

    def summary(self):
        if not c4u_options:
            return ""
        g = self.store.get
        resolution = g("scale")
        parts = [
            self.tr(f"Scale {resolution}x" if resolution != "auto" else "Scale auto", f"Escala {resolution}x" if resolution != "auto" else "Escala auto"),
            self.tr("Fullscreen", "Pantalla completa") if g("fullscreen") == "on" else self.tr("Window", "Ventana"),
            f"FPS {g('fps')}",
            "VSync " + g("vsync"),
            self.tr("HD textures", "Texturas HD") + " " + g("hd_textures"),
            self.tr("Layout", "Pantallas") + f" {g('mode')}",
        ]
        return "  •  ".join(parts)

    def refresh_saves_label(self):
        if not hasattr(self, "lbl_backups"):
            return
        backups = self.saves_backups(self.store.values)
        if backups:
            import time

            newest = time.strftime("%Y-%m-%d %H:%M", time.localtime(backups[0][1]))
            self.lbl_backups.setText(self.tr(f"{len(backups)} automatic backups. The newest: {backups[0][0]} ({newest}).",
                                             f"{len(backups)} copias automáticas. La más reciente: {backups[0][0]} ({newest})."))
        else:
            self.lbl_backups.setText(self.tr("No automatic backups yet: the game makes one when you start it.", "Aún no hay copias automáticas: el juego hace una al abrirlo."))

    def refresh_log(self):
        if not hasattr(self, "txt_log"):
            return
        text = self.tail_log(40)
        if text != getattr(self, "_log_text", None):
            self._log_text = text
            self.txt_log.setPlainText(text)
            sb = self.txt_log.verticalScrollBar()
            sb.setValue(sb.maximum())

    def _option_changed(self, key):
        self.lbl_summary.setText(self.summary())

    # ---- actions -----------------------------------------------------------------------------------------------------------
    def on_play(self):
        err = self.launch_game(on_exit=lambda code: self.ui_queue.put(("game_exit", code)))
        if err:
            qtshim.messagebox.showerror("Caravan4URecomp", self.tr(f"The game could not start: {err}", f"No se pudo abrir el juego: {err}"))
            return
        self.refresh_status()
        if self.config.get("minimize_on_play", True):
            QTimer.singleShot(600, self.window.showMinimized)

    def on_setup(self):
        err = self.launch_game(["--setup"], on_exit=lambda code: self.ui_queue.put(("game_exit", code)))
        if err:
            qtshim.messagebox.showerror("Caravan4URecomp", self.tr(f"The game could not start: {err}", f"No se pudo abrir el juego: {err}"))
        else:
            self.refresh_status()

    def open_textures(self):
        path = os.path.join(self.user_dir(self.store.values), "load", "textures")
        try:
            os.makedirs(path, exist_ok=True)
        except OSError:
            pass
        self.open_folder(path)

    def _game_exited(self, code):
        self.refresh_status()
        if self.window.isMinimized():
            self.window.showNormal()
            self.window.activateWindow()

    def on_app_close(self):
        self.poll.stop()
        self.log_timer.stop()

    # ---- updates -----------------------------------------------------------------------------------------------------------
    def on_update_button(self):
        if self.update_info is not None:
            self.install_update()
        else:
            self.check_for_updates(False)

    def check_for_updates(self, silent=False):
        if not c4u_updater:
            if not silent:
                qtshim.messagebox.showinfo("Caravan4URecomp", self.tr("This build has no updater.", "Esta versión no incluye el actualizador."))
            return
        if self._busy_update:
            return
        self._busy_update = True
        self.btn_update.configure(state="disabled")
        self.ui_queue.put(("update_state", self.tr("Looking for updates...", "Buscando actualizaciones...")))

        def work():
            try:
                info = c4u_updater.check(self.version, self.base_dir)
                self.ui_queue.put(("update_available", info))
            except Exception as e:
                text = getattr(e, "message_es" if self.language() == "es" else "message_en", None) or str(e)
                if "404" in str(e) or "404" in text:  # no release with a manifest yet: nothing to update to
                    text = self.tr("No updates published yet.", "Aún no hay actualizaciones publicadas.")
                self.ui_queue.put(("update_state", "" if silent else text))
                self.after(0, lambda: self.btn_update.configure(state="normal"))
            finally:
                self._busy_update = False

        threading.Thread(target=work, daemon=True).start()

    def _show_update(self, info):
        self.btn_update.configure(state="normal")
        if info is None:
            self.update_info = None
            self.btn_update.configure(text=self.tr("⟳  Check for updates", "⟳  Buscar actualizaciones"))
            self.lbl_update_status.setText(self.tr("You have the latest version.", "Tienes la última versión."))
            return
        self.update_info = info
        size = getattr(info, "total_bytes", 0) / 1048576
        self.btn_update.configure(text=self.tr(f"⬇  Update to {info.version}", f"⬇  Actualizar a {info.version}"))
        self.lbl_update_status.setText(self.tr(f"New version {info.version} ({size:.0f} MB to download).", f"Versión nueva {info.version} ({size:.0f} MB por bajar)."))

    def install_update(self):
        info = self.update_info
        if info is None or self._busy_update:
            return
        if self.is_running():
            qtshim.messagebox.showinfo("Caravan4URecomp", self.tr("Close the game before updating.", "Cierra el juego antes de actualizar."))
            return
        notes = (getattr(info, "notes_es", "") if self.language() == "es" else getattr(info, "notes_en", "")) or ""
        text = self.tr(f"Update to {info.version}?\n\n{notes}\n\nThe launcher closes, applies the changed files and opens again. Your settings and saves are not touched.",
                       f"¿Actualizar a {info.version}?\n\n{notes}\n\nEl launcher se cierra, aplica los archivos que cambiaron y se abre de nuevo. Tus ajustes y partidas no se tocan.")
        if not qtshim.messagebox.askyesno("Caravan4URecomp", text):
            return
        self._busy_update = True
        self.btn_update.configure(state="disabled")
        self.progress.setVisible(True)
        self.ui_queue.put(("update_state", self.tr("Downloading...", "Descargando...")))

        def work():
            try:
                c4u_updater.download(info, self.base_dir, progress=lambda f, rel: self.ui_queue.put(("progress", f)))
                self.ui_queue.put(("update_state", self.tr("Applying... the launcher restarts.", "Aplicando... el launcher se reinicia.")))
                c4u_updater.apply(info, self.base_dir)
                self.after(400, self.qapp.quit)
            except Exception as e:
                msg = getattr(e, "message_es" if self.language() == "es" else "message_en", None) or str(e)
                self.ui_queue.put(("error", ("Caravan4URecomp", msg)))
                self.ui_queue.put(("update_state", ""))
                self.after(0, lambda: (self.btn_update.configure(state="normal"), self.progress.setVisible(False)))
            finally:
                self._busy_update = False

        threading.Thread(target=work, daemon=True).start()


def main():
    qapp = QApplication(sys.argv)
    qapp.setStyle(qtanim.TipStyle())
    qapp.setStyleSheet(STYLE)
    qtanim.install_tips(qapp)
    qapp.setFont(qtshim.font(10.5))
    qtshim.init_threading()
    app = C4ULauncherApp(qapp)
    qtanim.fade_window_in(app.window)
    app.show()
    QTimer.singleShot(0, lambda: qtanim.intro([app.header, app.tabview.bar, app.tabview.stack, app.footer]))
    return qapp.exec()


if __name__ == "__main__":
    sys.exit(main())
