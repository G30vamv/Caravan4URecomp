# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
# Our own code, written from: windows/ingame_menu.h, windows/menu_text.h, windows/win_main.cpp, windows/addr_parse.h and windows/package/LEEME.txt.
"""INI options understood by the Windows game and its launcher."""

from dataclasses import dataclass
import re


@dataclass(frozen=True)
class Option:
    key: str
    tab: str
    section: str
    kind: str
    default: str
    choices: tuple = ()
    min: int | None = None
    max: int | None = None
    step: int | None = None
    unit: str = ""
    restart: bool = False
    in_game: bool = True
    visible: bool = True
    label_en: str = ""
    label_es: str = ""
    help_en: str = ""
    help_es: str = ""
    depends_on: str = ""


TABS: tuple[tuple[str, str, str], ...] = (
    ("graphics", "Graphics", "Gráficos"),
    ("game", "Game", "Juego"),
    ("controls", "Controls", "Controles"),
    ("audio", "Audio", "Audio"),
    ("online", "Online", "En línea"),
    ("saves", "Saves", "Partidas"),
    ("system", "System", "Sistema"),
)

_DEFAULT = ("default", "Default", "Predeterminado")
_AUTO = ("auto", "Auto", "Auto")
_GAME_TOGGLE = (_DEFAULT, ("on", "On", "Sí"), ("off", "Off", "No"))
_INT_MAX = 2_147_483_647

OPTIONS: tuple[Option, ...] = (
    Option("fullscreen", "graphics", "Window", "bool", "off",
           label_en="Fullscreen", label_es="Pantalla completa",
           help_en="Borderless fullscreen on the display the window is on. F11 or Alt+Enter does the same.",
           help_es="Pantalla completa sin bordes en la pantalla donde está la ventana. F11 o Alt+Enter hacen lo mismo."),
    Option("width", "graphics", "Window", "int", "1200",
           min=320, max=_INT_MAX, step=1, unit="px", restart=True, in_game=False,
           label_en="Window width", label_es="Ancho de la ventana",
           help_en="Initial width of the window in windowed mode.",
           help_es="Ancho inicial de la ventana en modo ventana.", depends_on="fullscreen=off"),
    Option("height", "graphics", "Window", "int", "720",
           min=240, max=_INT_MAX, step=1, unit="px", restart=True, in_game=False,
           label_en="Window height", label_es="Alto de la ventana",
           help_en="Initial height of the window in windowed mode.",
           help_es="Alto inicial de la ventana en modo ventana.", depends_on="fullscreen=off"),
    Option("display", "graphics", "Window", "int", "-1",
           min=-1, max=_INT_MAX, step=1, restart=True, in_game=False,
           label_en="Startup display", label_es="Pantalla de inicio",
           help_en="Display index in SDL order, starting at 0. Use -1 to select the primary display in normal play.",
           help_es="Índice de pantalla en el orden de SDL, a partir de 0. Usa -1 para seleccionar la pantalla principal al jugar."),
    Option("vsync", "graphics", "Window", "bool", "on",
           label_en="VSync", label_es="VSync",
           help_en="Shows each frame when your display refreshes. Off lets the game run above your display's rate, and the picture may tear.",
           help_es="Muestra cada fotograma cuando se refresca tu pantalla. Apagado deja que el juego supere su frecuencia, y la imagen puede cortarse."),
    Option("mode", "graphics", "Screen layout", "choice", "popup",
           choices=(("popup", "Popup", "Recuadro"), ("stacked", "Stacked", "Apiladas"),
                    ("overlay", "Overlay", "Superpuestas"), ("single", "Single", "Una sola"),
                    ("side", "Side", "Lado a lado"), ("large", "Large", "Grande y pequeña")),
           label_en="Screen layout", label_es="Disposición de pantallas",
           help_en="Popup: the top screen fills the window and the touch screen pops up over it when the game needs it. The others show both screens all the time.",
           help_es="Recuadro: la pantalla de arriba llena la ventana y la táctil aparece encima cuando el juego la necesita. Las demás muestran las dos pantallas siempre."),
    Option("touch_show", "graphics", "Screen layout", "choice", "auto",
           choices=(_AUTO, ("always", "Always", "Siempre"), ("hidden", "Hidden", "Oculta")),
           label_en="Touch screen shows", label_es="Pantalla táctil",
           help_en="When the touch screen's popup appears: when the game needs it (auto), always, or never. F2 shows or hides it by hand.",
           help_es="Cuándo aparece el recuadro de la pantalla táctil: cuando el juego la necesita (auto), siempre o nunca. F2 lo muestra u oculta a mano.",
           depends_on="mode=popup"),
    Option("popup_size", "graphics", "Screen layout", "int", "38",
           min=15, max=80, step=5, unit="%",
           label_en="Touch popup size", label_es="Tamaño del recuadro táctil",
           help_en="The height of the touch screen's popup, as a percentage of the window's height.",
           help_es="La altura del recuadro de la pantalla táctil, como porcentaje de la altura de la ventana.",
           depends_on="mode=popup"),
    Option("popup_corner", "graphics", "Screen layout", "choice", "bottom-right",
           choices=(("bottom-right", "Bottom-right", "Abajo a la derecha"),
                    ("bottom-left", "Bottom-left", "Abajo a la izquierda"),
                    ("top-right", "Top-right", "Arriba a la derecha"),
                    ("top-left", "Top-left", "Arriba a la izquierda")),
           label_en="Touch popup corner", label_es="Esquina del recuadro táctil",
           help_en="The corner of the window where the touch screen's popup sits.",
           help_es="La esquina de la ventana donde se pone el recuadro táctil.", depends_on="mode=popup"),
    Option("aspect", "graphics", "Screen layout", "choice", "default",
           choices=(_DEFAULT, ("stretch", "Stretch", "Estirar"),
                    *((ratio, ratio, ratio) for ratio in ("16:9", "16:10", "21:9", "4:3"))),
           label_en="Picture shape", label_es="Forma de la imagen",
           help_en="Default keeps the 3DS's own 5:3 shape (black bars at the sides on a wide screen). Stretch fills the whole window. The ratios draw the picture in that shape.",
           help_es="Default mantiene la forma 5:3 de la 3DS (barras negras a los lados en una pantalla ancha). Stretch llena toda la ventana. Las proporciones dibujan la imagen con esa forma."),
    Option("scale", "graphics", "Rendering", "choice", "3", unit="x", restart=True,
           choices=(_AUTO, *((str(n), f"{n}x  ({400 * n} × {240 * n})",
                              f"{n}x  ({400 * n} × {240 * n})") for n in range(1, 9))),
           label_en="Internal resolution", label_es="Resolución interna",
           help_en="How sharp the 3D scene, the HUD and the text are drawn: a multiple of the 3DS top screen's 400 x 240. Auto picks the one closest to your window.",
           help_es="Qué tan nítidos se dibujan la escena 3D, el HUD y el texto: un múltiplo de la resolución de la 3DS. Auto elige el más cercano a tu ventana."),
    Option("fps_counter", "graphics", "Rendering", "bool", "off",
           label_en="FPS counter", label_es="Contador de FPS",
           help_en="Shows the game's frame rate in the top-left corner.",
           help_es="Muestra los fotogramas por segundo del juego en la esquina superior izquierda."),
    Option("hd_textures", "graphics", "Rendering", "bool", "on", restart=True, in_game=False,
           label_en="HD textures", label_es="texturas HD",
           help_en="Loads HD texture packs (Azahar or Citra format) when the game starts.",
           help_es="Carga los paquetes de texturas HD (formato Azahar o Citra) al abrir el juego."),
    Option("gpu_thread", "graphics", "Rendering", "bool", "off", restart=True, in_game=False,
           label_en="Dedicated GPU thread", label_es="Hilo dedicado para la GPU",
           help_en="Runs GPU work on its own thread.", help_es="Ejecuta el trabajo de la GPU en su propio hilo."),
    Option("fps", "game", "Frame rate", "choice", "default", unit="Hz",
           choices=(_DEFAULT, ("display", "Display", "Tu pantalla"),
                    *((str(n), str(n), str(n)) for n in (30, 60, 120, 180, 240, 300, 360)),
                    ("unlimited", "Unlimited", "Sin límite")),
           label_en="Frame rate limit", label_es="Límite de fotogramas",
           help_en="Caps the game's frame rate. Default is the game's own, Display matches your screen, Unlimited uses all the CPU.",
           help_es="Limita los fotogramas del juego. Predeterminado es el del juego, Tu pantalla iguala su frecuencia, Sin límite usa toda la CPU."),
    Option("steady", "game", "Frame rate", "choice", "default", choices=_GAME_TOGGLE,
           label_en="Steady frame rate", label_es="Fotogramas estables",
           help_en="Keeps the frame rate even instead of letting it vary. Default leaves the game's own setting.",
           help_es="Mantiene los fotogramas parejos en vez de dejarlos variar. Predeterminado deja el ajuste del juego."),
    Option("effects", "game", "Effects", "choice", "default", choices=_GAME_TOGGLE,
           label_en="Particle effects", label_es="Efectos de partículas",
           help_en="Turns the game's particle effects on or off. Default leaves the game's own setting.",
           help_es="Activa o desactiva los efectos de partículas del juego. Predeterminado deja el ajuste del juego."),
    Option("pace", "game", "Frame rate", "choice", "vblank", restart=True, in_game=False, visible=False,
           choices=(("vblank", "VBlank", "VBlank"), ("limiter", "Limiter", "Limitador")),
           label_en="Frame pacing", label_es="Ritmo de fotogramas",
           help_en="Selects VBlank pacing or the game's own frame limiter.",
           help_es="Selecciona el ritmo de VBlank o el limitador de fotogramas del juego."),
    Option("skip_key", "controls", "Skip a cutscene", "key", "Tab",
           label_en="Skip a cutscene (key)", label_es="Saltar cinemática (tecla)",
           help_en="The key and the controller's button that tap the Skip button of a cutscene. They keep doing what they do in the game too.",
           help_es="La tecla y el botón del mando que pulsan Omitir en una cinemática. Siguen haciendo lo suyo en el juego."),
    Option("skip_pad", "controls", "Skip a cutscene", "padbutton", "",
           label_en="Skip a cutscene (controller)", label_es="Saltar cinemática (mando)",
           help_en="The key and the controller's button that tap the Skip button of a cutscene. They keep doing what they do in the game too.",
           help_es="La tecla y el botón del mando que pulsan Omitir en una cinemática. Siguen haciendo lo suyo en el juego."),
    Option("circle_pad_pro", "controls", "Input devices", "bool", "on", restart=True, in_game=False,
           label_en="Circle Pad Pro", label_es="Circle Pad Pro",
           help_en="Enables the emulated Circle Pad Pro.", help_es="Activa el Circle Pad Pro emulado."),
    Option("mouse_camera", "controls", "Input devices", "bool", "off", in_game=False,
           label_en="Mouse camera", label_es="Cámara con ratón",
           help_en="Uses the mouse for the camera. Hold Left Alt to use the stylus, or press F3 to toggle the mouse mode.",
           help_es="Usa el ratón para la cámara. Mantén pulsado Alt izquierdo para usar el lápiz táctil o presiona F3 para cambiar de modo."),
    Option("hid_pads", "controls", "Input devices", "bool", "on", restart=True, in_game=False,
           label_en="HID controllers", label_es="Mandos HID",
           help_en="Enables direct HID support for compatible controllers.",
           help_es="Activa la compatibilidad directa con mandos HID."),
    Option("hidhide_ask", "controls", "Input devices", "bool", "on", restart=True, in_game=False,
           label_en="Ask about HidHide", label_es="Preguntar por HidHide",
           help_en="Asks at startup when HidHide prevents the game from reading a compatible controller.",
           help_es="Pregunta al iniciar si HidHide impide que el juego lea un mando compatible.", depends_on="hid_pads=on"),
    Option("volume", "audio", "Game audio", "int", "50", min=0, max=100, step=5, unit="%",
           label_en="Volume", label_es="Volumen",
           help_en="The volume of the game.", help_es="El volumen del juego."),
    Option("nick", "online", "Rooms", "text", "",
           label_en="Nickname", label_es="Apodo",
           help_en="Your name in network rooms: 4 to 20 letters, digits, spaces, dots, underscores or dashes.",
           help_es="Tu nombre en las salas en línea: de 4 a 20 letras, dígitos, espacios, puntos, guiones o guiones bajos."),
    Option("join_address", "online", "Rooms", "text", "",
           label_en="Address HOST:PORT", label_es="Dirección HOST:PUERTO",
           help_en="The address of a room, like 203.0.113.7:24872.",
           help_es="La dirección de una sala, como 203.0.113.7:24872."),
    Option("host_port", "online", "Rooms", "int", "24872", min=1, max=65535, step=1,
           label_en="Host port (UDP)", label_es="Puerto para alojar (UDP)",
           help_en="The UDP port your room listens on. Open it on your router so others can join.",
           help_es="El puerto UDP de tu sala. Ábrelo en tu router para que otros entren."),
    Option("lobby", "online", "Rooms", "text", "http://88.198.47.46:5000", in_game=False,
           label_en="Public lobby URL", label_es="URL del lobby público",
           help_en="HTTP address used to fetch the public room list.",
           help_es="Dirección HTTP que se usa para consultar la lista de salas públicas."),
    Option("online_touch", "online", "Touch screen", "bool", "on", in_game=False,
           label_en="Touch popup in online rooms", label_es="Recuadro táctil en salas en línea",
           help_en="In automatic popup mode, keeps the touch screen visible in town while connected to a room.",
           help_es="En modo de recuadro automático, mantiene visible la pantalla táctil en el pueblo mientras estás en una sala.",
           depends_on="touch_show=auto"),
    Option("auto_backup", "saves", "Backups", "bool", "on", restart=True,
           label_en="Back up automatically at start", label_es="Copia automática al abrir el juego",
           help_en="Backs up your saves each time the game starts, if they changed since the last backup. The newest 5 automatic backups are kept; yours are never deleted.",
           help_es="Copia tus guardados cada vez que abres el juego, si cambiaron desde la última copia. Se guardan las 5 automáticas más nuevas; las tuyas nunca se borran."),
    Option("language", "system", "Menu", "choice", "auto",
           choices=(_AUTO, ("en", "English", "English"), ("es", "Español", "Español")),
           label_en="Language", label_es="Idioma",
           help_en="The language of this menu. Auto follows Windows.",
           help_es="El idioma de este menú. Auto sigue el de Windows."),
    Option("discord_presence", "system", "Integration", "bool", "on", restart=True, in_game=False,
           label_en="Discord Rich Presence", label_es="Presencia enriquecida de Discord",
           help_en="Shows the game and elapsed play time in Discord.",
           help_es="Muestra el juego y el tiempo de juego transcurrido en Discord."),
    Option("trace_panels", "system", "Diagnostics", "bool", "off", restart=True, in_game=False, visible=False,
           label_en="Trace panels", label_es="Registrar paneles",
           help_en="Enables touch panel tracing.", help_es="Activa el registro de los paneles táctiles."),
    Option("options", "system", "Game files", "text", "", restart=True, in_game=False, visible=False,
           label_en="Runner options", label_es="Opciones de ejecución",
           help_en="Extra command-line arguments for the runner.",
           help_es="Argumentos adicionales de línea de comandos para el ejecutor."),
    Option("game", "system", "Game files", "path", "", restart=True, in_game=False, visible=False,
           label_en="Game folder", label_es="Carpeta del juego",
           help_en="Folder containing the prepared game.", help_es="Carpeta que contiene el juego preparado."),
    Option("code", "system", "Game files", "path", "", restart=True, in_game=False, visible=False,
           label_en="Code library", label_es="Librería de código",
           help_en="Path to the game's recompiled library.", help_es="Ruta a la librería recompilada del juego."),
    Option("user", "system", "Game files", "path", "", restart=True, in_game=False, visible=False,
           label_en="User folder", label_es="Carpeta de usuario",
           help_en="User folder for saves and other game data.",
           help_es="Carpeta de usuario para guardados y otros datos del juego."),
)

_BY_KEY = {option.key: option for option in OPTIONS}

# The mapper builds these names at runtime. They belong to the F1 button mapper.
INTERNAL_KEYS = frozenset(
    f"{device}_{button}"
    for device in ("key", "pad")
    for button in ("a", "b", "x", "y", "l", "r", "zl", "zr", "start", "select",
                   "up", "down", "left", "right")
)


def by_key(key) -> Option:
    return _BY_KEY[key]


def by_tab(tab) -> list[Option]:
    return [option for option in OPTIONS if option.tab == tab]


def _integer(value: str) -> int | None:
    if not re.fullmatch(r"[+-]?[0-9]+", value):
        return None
    digits = value.lstrip("+-").lstrip("0") or "0"
    if len(digits) > 10:
        return None
    return int(("-" if value.startswith("-") else "") + digits)


def validate(key, value) -> tuple[bool, str]:
    """Validate a launcher edit. Reading the INI leaves stored values intact."""
    option = _BY_KEY.get(key)
    if option is None:
        return False, f"Unknown option: {key}"
    if type(value) is bool and option.kind == "bool":
        return True, "on" if value else "off"
    if type(value) is int:
        try:
            value = str(value)
        except ValueError:
            return False, "The number is too large."
    if not isinstance(value, str) or any(c in value for c in "\r\n\0"):
        return False, "Use a single line of text."
    value = value.strip(" \t")

    if option.kind == "bool":
        setting = value.lower()
        if setting in ("on", "true", "1"):
            return True, "on"
        if setting in ("off", "false", "0"):
            return True, "off"
        return False, "Choose on or off."

    if option.kind == "choice":
        setting = value.lower()
        if key == "scale" and setting in ("9", "10"):
            return True, "8"
        allowed = tuple(choice[0] for choice in option.choices)
        if setting in allowed:
            return True, setting
        return False, "Choose one of: " + ", ".join(allowed)

    if option.kind == "int":
        if key == "host_port" and not value:
            return True, option.default
        number = _integer(value)
        if number is not None and option.min <= number <= option.max:
            return True, str(number)
        return False, f"Enter an integer from {option.min} to {option.max}."

    if option.kind == "key":
        # Actual key names are resolved by SDL in the game.
        if not value or value.lower() == "tab":
            return True, "Tab"
        return True, "none" if value.lower() == "none" else value

    if option.kind == "padbutton":
        value = value.lower()
        if value in ("", "none"):
            return True, ""
        bind = re.fullmatch(r"b(?P<button>[0-9]+)|a(?P<axis>[0-9]+)(?P<direction>[+-])", value)
        if bind:
            index = _integer(bind["button"] or bind["axis"])
            if index is not None and index <= _INT_MAX:
                return True, f"{value[0]}{index}{bind['direction'] or ''}"
        return False, "Use bN for a button or aN+ / aN- for an axis."

    if key == "nick" and value and not re.fullmatch(r"[A-Za-z0-9 ._-]{4,20}", value):
        return False, "Use 4 to 20 letters, digits, spaces, dots, underscores or dashes."

    if key == "join_address" and value:
        address = re.split(r"[ \t]", value, maxsplit=1)[0]
        host, separator, port = address.partition(":")
        if len(address) > 255 or not re.fullmatch(r"[A-Za-z0-9.-]+", host):
            return False, "Enter a host name or an IPv4 address, optionally followed by :PORT."
        if separator:
            if not re.fullmatch(r"[0-9]{1,5}", port) or not 1 <= int(port) <= 65535:
                return False, "Enter a port from 1 to 65535."
            return True, f"{host}:{int(port)}"
        return True, host

    return True, value
