# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
"""The pages of options of the launcher, made from the schema in c4u_options.py (one page per tab, one card per section).

Every change is written to Caravan4URecomp.ini at once (the game is closed while the launcher is open, so everything can be changed here,
also what the in-game menu can only apply after a restart).
"""

from PySide6.QtCore import Qt
from PySide6.QtGui import QKeySequence
from PySide6.QtWidgets import QHBoxLayout, QPushButton, QScrollArea, QVBoxLayout, QWidget

import qtanim
import qtshim
from qtglass import GlassPanel
from qtshim import Button, Entry, Label, OptionMenu, Slider, Switch

MUTED = "#c7b48f"
CREAM = "#e3d8bd"
GOLD_TEXT = "#eab84c"

# the buttons a controller can give (SDL's names), as the value the game's ini keeps: b<N> a button, a<N>+ an axis pulled
PAD_CHOICES = [
    ("", "None", "Ninguno"),
    ("b0", "South (Xbox A / PlayStation Cross)", "Sur (A de Xbox / Cruz de PlayStation)"),
    ("b1", "East (Xbox B / PlayStation Circle)", "Este (B de Xbox / Círculo de PlayStation)"),
    ("b2", "West (Xbox X / PlayStation Square)", "Oeste (X de Xbox / Cuadrado de PlayStation)"),
    ("b3", "North (Xbox Y / PlayStation Triangle)", "Norte (Y de Xbox / Triángulo de PlayStation)"),
    ("b4", "Back / Select / Share", "Atrás / Select / Share"),
    ("b6", "Start / Options", "Start / Options"),
    ("b7", "Left stick click (L3)", "Clic del stick izquierdo (L3)"),
    ("b8", "Right stick click (R3)", "Clic del stick derecho (R3)"),
    ("b9", "Left bumper (L1 / LB)", "Bumper izquierdo (L1 / LB)"),
    ("b10", "Right bumper (R1 / RB)", "Bumper derecho (R1 / RB)"),
    ("a4+", "Left trigger (L2 / LT)", "Gatillo izquierdo (L2 / LT)"),
    ("a5+", "Right trigger (R2 / RT)", "Gatillo derecho (R2 / RT)"),
    ("b11", "D-pad up", "Cruceta arriba"),
    ("b12", "D-pad down", "Cruceta abajo"),
    ("b13", "D-pad left", "Cruceta izquierda"),
    ("b14", "D-pad right", "Cruceta derecha"),
]

# Qt's name of a key -> SDL's (the game finds the key with SDL_GetKeyFromName, which ignores case)
_QT_TO_SDL = {"Esc": "Escape", "Return": "Return", "Enter": "Return", "Backspace": "Backspace", "Del": "Delete", "Ins": "Insert",
              "PgUp": "PageUp", "PgDown": "PageDown", "Caps Lock": "CapsLock", "Space": "Space", "Tab": "Tab"}


class KeyCapture(QPushButton):
    """A button that shows a key and, when pressed, takes the next key as the new one (Esc cancels)."""

    def __init__(self, value, on_change, tr, parent=None):
        super().__init__(parent)
        self._tr = tr
        self._value = value
        self._on_change = on_change
        self._waiting = False
        self.setMinimumHeight(34)
        self.setMinimumWidth(170)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFont(qtshim.font(10.5, True))
        self.setStyleSheet(
            "QPushButton { background: rgba(13,8,4,185); color: #fff6e0; border: 1px solid rgba(255,255,255,45); border-radius: 9px; padding: 4px 12px; }"
            "QPushButton:hover { border: 1px solid #d4922a; }"
        )
        self.clicked.connect(self._start)
        self._show()

    def _show(self):
        if self._waiting:
            self.setText(self._tr("Press a key...", "Pulsa una tecla..."))
        else:
            self.setText(self._value or self._tr("None", "Ninguna"))

    def _start(self):
        self._waiting = True
        self._show()
        self.setFocus()
        self.grabKeyboard()

    def set(self, value):
        self._value = value
        self._waiting = False
        self._show()

    def get(self):
        return self._value

    def keyPressEvent(self, event):
        if not self._waiting:
            return super().keyPressEvent(event)
        self.releaseKeyboard()
        self._waiting = False
        if event.key() != Qt.Key.Key_Escape:
            name = QKeySequence(event.key()).toString()
            name = _QT_TO_SDL.get(name, name)
            if name:
                self._value = name
                self._on_change(name)
        self._show()
        event.accept()

    def focusOutEvent(self, event):
        if self._waiting:
            self.releaseKeyboard()
            self._waiting = False
            self._show()
        super().focusOutEvent(event)


class OptionsForm:
    """Builds the pages and keeps the widgets in step with the ini."""

    def __init__(self, schema, store, lang_fn, on_changed=None):
        self.schema = schema  # the module c4u_options
        self.store = store    # .get(key) -> str (effective), .set(key, value)
        self.lang = lang_fn
        self.on_changed = on_changed or (lambda key: None)
        self.widgets = {}     # key -> (kind, widget)
        self.rows = {}        # key -> row widget (hidden / disabled by depends_on)
        self._quiet = False

    def tr(self, en, es):
        return es if self.lang() == "es" else en

    def label_of(self, opt):
        return (opt.label_es or opt.label_en or opt.key) if self.lang() == "es" else (opt.label_en or opt.key)

    def help_of(self, opt):
        return (opt.help_es or opt.help_en) if self.lang() == "es" else opt.help_en

    # ---- pages -----------------------------------------------------------------------------------------------------------
    def build_page(self, tab):
        options = [o for o in self.schema.by_tab(tab) if o.visible]
        inner = QWidget()
        lay = QVBoxLayout(inner)
        lay.setContentsMargins(4, 4, 10, 4)
        lay.setSpacing(12)
        section = None
        card = card_lay = None
        for opt in options:
            if opt.section != section or card is None:
                section = opt.section
                card = GlassPanel("card", 16)
                card_lay = QVBoxLayout(card)
                card_lay.setContentsMargins(18, 14, 18, 14)
                card_lay.setSpacing(10)
                title = Label(section.upper(), GOLD_TEXT, 10, bold=True)
                card_lay.addWidget(title)
                lay.addWidget(card)
            card_lay.addWidget(self._row(opt))
        reset = Button(self.tr("Restore defaults of this page", "Restaurar los valores de esta página"), "default",
                       lambda t=tab: self.reset_tab(t), height=32, size=10)
        lay.addWidget(reset, 0, Qt.AlignmentFlag.AlignLeft)
        lay.addStretch(1)
        area = QScrollArea()
        area.setWidgetResizable(True)
        area.setFrameShape(QScrollArea.Shape.NoFrame)
        area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        area.viewport().setAutoFillBackground(False)
        inner.setAutoFillBackground(False)
        area.setWidget(inner)
        self.refresh_dependencies()
        return area

    def _row(self, opt):
        row = QWidget()
        h = QHBoxLayout(row)
        h.setContentsMargins(0, 0, 0, 0)
        h.setSpacing(14)
        current = self.store.get(opt.key)
        text = self.label_of(opt)
        tip = self.help_of(opt)
        if opt.restart:
            tip = (tip + "\n\n" if tip else "") + self.tr("Applies the next time the game starts.", "Se aplica la próxima vez que abras el juego.")
        key = opt.key
        if opt.kind == "bool":
            w = Switch(text, lambda k=key: self._changed(k))
            self._set_toggle(w, current == "on")
            h.addWidget(w, 1)
            self.widgets[key] = ("bool", w)
            self._tip(tip, w)
        else:
            lab = Label(text, CREAM, 10.5)
            h.addWidget(lab, 1)
            self._tip(tip, lab)
            if opt.kind == "choice":
                w = OptionMenu([])
                w.clear()
                for value, en, es in opt.choices:
                    w.addItem(es if self.lang() == "es" else en, value)
                w.set(current)
                w.setMinimumWidth(260)
                w.currentIndexChanged.connect(lambda _i, k=key: self._changed(k))
                self.widgets[key] = ("choice", w)
                self._tip(tip, w)
                h.addWidget(w)
            elif opt.kind == "padbutton":
                w = OptionMenu([])
                w.clear()
                for value, en, es in PAD_CHOICES:
                    w.addItem(es if self.lang() == "es" else en, value)
                if current and w.findData(current) < 0:
                    w.addItem(current, current)
                w.set(current)
                w.setMinimumWidth(300)
                w.currentIndexChanged.connect(lambda _i, k=key: self._changed(k))
                self.widgets[key] = ("padbutton", w)
                self._tip(tip, w)
                h.addWidget(w)
            elif opt.kind == "int":
                step = opt.step or 1
                lo, hi = opt.min if opt.min is not None else 0, opt.max if opt.max is not None else 100
                steps = max(1, (hi - lo) // step)
                if steps > 400:  # a wide range (a port, a window size): a number box, not a slider
                    w = Entry("", 130)
                    w.setText(current)
                    w.editingFinished.connect(lambda k=key: self._changed(k))
                    h.addWidget(w)
                    self.widgets[key] = ("number", w)
                    self._tip(tip, w)
                    self.rows[key] = row
                    return row
                w = Slider(0, steps)
                w.setMinimumWidth(240)
                value_label = Label("", GOLD_TEXT, 10.5, bold=True)
                value_label.setMinimumWidth(70)

                def moved(index, k=key, lo=lo, step=step, unit=opt.unit, vl=value_label):
                    vl.setText(f"{lo + index * step}{unit}")
                    if not self._quiet:
                        self._changed(k)

                w._command = moved
                try:
                    v = int(float(current))
                except ValueError:
                    v = lo
                w.set(round((min(max(v, lo), hi) - lo) / step))
                value_label.setText(f"{min(max(v, lo), hi)}{opt.unit}")
                h.addWidget(w)
                h.addWidget(value_label)
                self.widgets[key] = ("int", (w, lo, step))
                self._tip(tip, w)
            elif opt.kind == "key":
                w = KeyCapture(current, lambda v, k=key: self._changed(k), self.tr)
                h.addWidget(w)
                self.widgets[key] = ("key", w)
                self._tip(tip, w)
            else:  # text
                w = Entry("", 300)
                w.setText(current)
                w.editingFinished.connect(lambda k=key: self._changed(k))
                h.addWidget(w)
                self.widgets[key] = ("text", w)
                self._tip(tip, w)
        self.rows[key] = row
        return row

    @staticmethod
    def _tip(text, *widgets):
        if not text:
            return
        for w in widgets:
            w.setToolTip(qtanim.tip_html(text))

    @staticmethod
    def _set_toggle(w, on):
        if hasattr(w, "select"):
            w.select() if on else w.deselect()
        else:
            w.setChecked(bool(on))

    # ---- values -------------------------------------------------------------------------------------------------------------
    def read(self, key):
        kind, w = self.widgets[key]
        if kind == "bool":
            return "on" if w.isChecked() else "off"
        if kind in ("choice", "padbutton"):
            return str(w.get())
        if kind == "int":
            slider, lo, step = w
            return str(lo + slider.value() * step)
        if kind == "key":
            return w.get()
        if kind == "number":
            return w.text().strip()
        return w.text().strip()

    def write(self, key, value):
        """Shows `value` in the widget of `key` (without writing it again)."""
        kind, w = self.widgets[key]
        self._quiet = True
        try:
            if kind == "bool":
                self._set_toggle(w, value == "on")
            elif kind in ("choice", "padbutton"):
                w.set(value)
            elif kind == "int":
                slider, lo, step = w
                try:
                    slider.set(round((int(float(value)) - lo) / step))
                except ValueError:
                    pass
            elif kind == "key":
                w.set(value)
            else:  # text and number
                w.setText(value)
        finally:
            self._quiet = False

    def _changed(self, key):
        if self._quiet or key not in self.widgets:
            return
        opt = self.schema.by_key(key)
        value = self.read(key)
        ok, normal = self.schema.validate(key, value)
        if not ok:
            return
        self.store.set(key, normal)
        self.refresh_dependencies()
        self.on_changed(key)

    def refresh_dependencies(self):
        for key, row in self.rows.items():
            dep = self.schema.by_key(key).depends_on
            if not dep or "=" not in dep:
                continue
            other, wanted = dep.split("=", 1)
            row.setEnabled(self.store.get(other) == wanted)

    def reset_tab(self, tab):
        for opt in self.schema.by_tab(tab):
            if opt.key in self.widgets:
                self.store.set(opt.key, opt.default)
                self.write(opt.key, opt.default)
        self.refresh_dependencies()
        self.on_changed("")

    def reload_all(self):
        for key in self.widgets:
            self.write(key, self.store.get(key))
        self.refresh_dependencies()
