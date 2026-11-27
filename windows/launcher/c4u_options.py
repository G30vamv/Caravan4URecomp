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
    ("mods", "Mods", "Mods"),
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
           help_en="Which monitor the game opens on: 0 is the first, 1 the second... -1 uses your main monitor.",
           help_es="En qué monitor se abre el juego: 0 es el primero, 1 el segundo... -1 usa tu monitor principal."),
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
    Option("widescreen", "graphics", "Screen layout", "choice", "off", restart=True,
           choices=(("off", "Default (5:3)", "Predeterminado (5:3)"), ("match", "Match the window", "Igualar la ventana"),
                    *((ratio, ratio, ratio) for ratio in ("16:9", "16:10", "21:9"))),
           label_en="Widescreen", label_es="Pantalla ancha",
           help_en="Draws the top screen wider: more of the scene at the sides, not stretched. Match fits your window, or pick a shape. Needs a restart.",
           help_es="Dibuja la pantalla de arriba más ancha: se ve más escena a los lados, sin estirar. Igualar ajusta a tu ventana, o elige una forma. Pide reiniciar."),
    Option("hide_help", "graphics", "Screen layout", "bool", "off",
           label_en="Hide the help boxes", label_es="Ocultar los cuadros de ayuda",
           help_en="Hides the game's help box (the black box with a line of text a menu shows for the selected row). It overlaps some menus and, on the title screen, made the touch screen pop up for a single line.",
           help_es="Oculta el cuadro de ayuda del juego (el recuadro negro con una línea de texto que un menú muestra para la fila elegida). Se superpone a algunos menús y, en la pantalla de título, hacía salir la táctil por una sola línea."),
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
    Option("monster_hp", "mods", "Built-in mods", "bool", "off",
           label_en="Monster life bar", label_es="Barra de vida del monstruo",
           help_en="A mod of this program, not a graphics option: it shows the life of the large monster you hurt last, top right, with its name, and how much it drops as you hit it. It only reads the game's memory and changes nothing, so it is fine online.",
           help_es="Un mod de este programa, no una opción de gráficos: muestra la vida del monstruo grande al que golpeaste por última vez, arriba a la derecha, con su nombre, y cómo baja con cada golpe. Solo lee la memoria del juego y no cambia nada, así que es seguro en línea."),
    Option("monster_hp_size", "mods", "Built-in mods", "int", "100", min=50, max=200, step=10, unit="%",
           label_en="Life bar size", label_es="Tamaño de la barra de vida",
           help_en="The size of the monster's life plate, in percent of its normal size.",
           help_es="El tamaño de la placa de vida del monstruo, en porcentaje de su tamaño normal.",
           depends_on="monster_hp=on"),
    Option("monster_hp_pos", "mods", "Built-in mods", "choice", "right",
           choices=(("right", "Top right", "Arriba a la derecha"), ("left", "Top left", "Arriba a la izquierda"), ("middle", "Top middle", "Arriba en el centro")),
           label_en="Life bar position", label_es="Posición de la barra de vida",
           help_en="Where the monster's life plate (and the damage meter under it) goes.",
           help_es="Dónde van la placa de vida del monstruo (y el contador de daño que va debajo)."),
    Option("monster_hp_percent", "mods", "Built-in mods", "bool", "off",
           label_en="Percent of life on the bar", label_es="Porcentaje de vida en la barra",
           help_en="Writes the percent of life left over the bar of the monster's plate.",
           help_es="Escribe el porcentaje de vida que queda sobre la barra de la placa del monstruo.",
           depends_on="monster_hp=on"),
    Option("damage_meter", "mods", "Built-in mods", "bool", "off",
           label_en="Damage meter", label_es="Contador de daño",
           help_en="A mod of this program: under the monster's life plate it shows the damage the monster has taken (total, damage a second over the last 10 seconds, and the last hit). It counts every drop of the monster's life, whoever dealt it, so online it also counts the other hunters. It only reads the game's memory, so it is fine online.",
           help_es="Un mod de este programa: bajo la placa de vida del monstruo muestra el daño que ha recibido (total, daño por segundo en los últimos 10 segundos y el último golpe). Cuenta toda bajada de vida del monstruo, la cause quien la cause, así que en línea cuenta también a los demás cazadores. Solo lee la memoria del juego, así que es seguro en línea."),
    Option("damage_mult", "mods", "Cheats", "choice", "1",
           choices=(("1", "1x (off)", "1x (apagado)"), ("1.5", "1.5x", "1.5x"), ("2", "2x", "2x"), ("3", "3x", "3x"), ("5", "5x", "5x"), ("10", "10x", "10x")),
           label_en="Damage multiplier", label_es="Multiplicador de daño",
           help_en="A cheat: every drop of a large monster's life is multiplied. It changes the game's memory: it can break an online hunt or a save. Use it at your own risk. A monster is never killed by the multiplier itself: it is left with 1 life and your next hit kills it.",
           help_es="Un truco: cada bajada de vida de un monstruo grande se multiplica. Cambia la memoria del juego: puede romper una caza en línea o una partida guardada. Úsalo bajo tu propio riesgo. El multiplicador nunca mata al monstruo por sí solo: lo deja con 1 de vida y tu siguiente golpe lo mata."),
    Option("qol_hold_gather", "mods", "Built-in mods", "bool", "off",
           label_en="Hold A to gather", label_es="Mantener A para recoger",
           help_en="Hold A to carve, gather, mine and catch bugs again and again until you let go, instead of pressing it each time. It only changes the controls, so it is fine online.",
           help_es="Mantén A para despiezar, recoger, picar y atrapar bichos una y otra vez hasta que sueltes, en vez de pulsarla cada vez. Solo cambia los controles, así que es seguro en línea."),
    Option("invincible", "mods", "Cheats", "bool", "off",
           label_en="Invincible", label_es="Invencible",
           help_en="A cheat: your hunter takes no damage (the health bar does not go down). It changes the game's memory: it can break an online hunt or a save. Use it at your own risk.",
           help_es="Un truco: tu cazador no recibe daño (la barra de vida no baja). Cambia la memoria del juego: puede romper una caza en línea o una partida guardada. Úsalo bajo tu propio riesgo."),
    Option("inf_items", "mods", "Cheats", "bool", "off",
           label_en="Infinite items", label_es="Objetos infinitos",
           help_en="What you use one at a time in a hunt is not used up (a potion, a bomb, ammo). Combining, moving or selling items counts as usual. It changes the game's memory: it can break an online hunt or a save. Use it at your own risk.",
           help_es="Lo que usas de uno en uno en una caza no se gasta (una poción, una bomba, munición). Combinar, mover o vender objetos cuenta como siempre. Cambia la memoria del juego: puede romper una caza en línea o una partida guardada. Úsalo bajo tu propio riesgo."),
    Option("qol_flex_cancel", "mods", "Built-in mods", "bool", "off",
           label_en="Cancel the flex after a drink", label_es="Cancelar la pose al beber",
           help_en="After a potion or a ration, moving, rolling or any other action ends the flex at once. What you drank still works; standing still, the flex plays whole.",
           help_es="Tras una poción o una ración, moverte, rodar o cualquier otra acción termina la pose al instante. Lo que bebiste sigue funcionando; si te quedas quieto, la pose se ve completa."),
    Option("qol_sharpen_cancel", "mods", "Built-in mods", "bool", "off",
           label_en="Cancel sharpening with B", label_es="Cancelar el afilado con B",
           help_en="B while sharpening rolls out of it, as in later Monster Hunter games.",
           help_es="B mientras afilas te saca del afilado con una voltereta, como en juegos posteriores."),
    Option("qol_rewards_no_limit", "mods", "Built-in mods", "bool", "off",
           label_en="No time limit on the rewards", label_es="Sin límite de tiempo en las recompensas",
           help_en="The rewards screen after a quest no longer counts down: take your time choosing.",
           help_es="La pantalla de recompensas tras una misión ya no hace cuenta atrás: elige con calma."),
    Option("rewards_send_all", "mods", "Built-in mods", "bool", "off",
           label_en="Send all the rewards to the box", label_es="Enviar todas las recompensas al baúl",
           help_en="Choosing Send to Item Box on the rewards screen sends every reward that fits at once.",
           help_es="Elegir Enviar al baúl en la pantalla de recompensas envía de una vez todas las que quepan."),
    Option("antialiasing", "graphics", "Screen effects", "bool", "on",
           label_en="Anti-aliasing", label_es="Suavizado de bordes",
           help_en="The game's own anti-aliasing, which smooths the edges. Off saves its pass; at a high internal resolution the edges stay fine.",
           help_es="El suavizado de bordes del propio juego. Desactivado ahorra su pasada; con una resolución interna alta los bordes se ven bien."),
    Option("bloom", "graphics", "Screen effects", "bool", "on",
           label_en="Bloom", label_es="Resplandor (bloom)",
           help_en="The glow over the bright parts of the picture. Off: sharper, with more contrast, and a little faster.",
           help_es="El brillo sobre las partes claras de la imagen. Desactivado: más nítido, con más contraste y un poco más rápido."),
    Option("motion_blur", "graphics", "Screen effects", "bool", "on",
           label_en="Motion blur", label_es="Desenfoque de movimiento",
           help_en="The blending of earlier frames that some effects add.",
           help_es="La mezcla con cuadros anteriores que agregan algunos efectos."),
    Option("depth_of_field", "graphics", "Screen effects", "bool", "on",
           label_en="Depth of field", label_es="Profundidad de campo",
           help_en="The focus blur of cutscenes. For now they stay sharp with it. Off skips its work, a little faster.",
           help_es="El desenfoque de las cinemáticas. Por ahora se ven nítidas con él. Desactivado se salta su trabajo, un poco más rápido."),
    Option("radial_blur", "graphics", "Screen effects", "bool", "on",
           label_en="Radial blur", label_es="Desenfoque radial",
           help_en="The zoom blur around a roaring monster, and in the cutscenes that use it.",
           help_es="El desenfoque de zoom alrededor de un monstruo que ruge, y en las cinemáticas que lo usan."),
    Option("haze", "graphics", "Screen effects", "bool", "on",
           label_en="Heat haze", label_es="Reverberación del calor",
           help_en="The heat shimmer of hot areas.",
           help_es="La reverberación del calor en las zonas calientes."),
    Option("dither", "graphics", "Screen effects", "bool", "on",
           label_en="Dither", label_es="Dither (tramado)",
           help_en="The game's dither pattern, which hides banding on the 3DS's screens. Above 1x internal resolution it can show as a fine grid: turn it off.",
           help_es="El patrón de tramado del juego, que oculta las bandas en las pantallas de la 3DS. Con resolución interna mayor que 1x puede verse como una rejilla fina: desactívalo."),
    Option("fov", "graphics", "Screen effects", "choice", "1.0",
           choices=tuple((v, v, v) for v in ("0.7", "0.8", "0.9", "1.0", "1.1", "1.2", "1.3", "1.4", "1.5")),
           label_en="Field of view", label_es="Campo de visión",
           help_en="How much of the scene you see: higher shows more around the hunter, lower less. 1.0 is the 3DS's. It applies at once.",
           help_es="Cuánto de la escena ves: más alto muestra más alrededor del cazador, más bajo menos. 1.0 es el de la 3DS. Se aplica al instante."),
    Option("kick", "controls", "Extra buttons", "choice", "off",
           choices=(("off", "Off", "No"), ("ZL", "ZL", "ZL"), ("ZR", "ZR", "ZR"), ("L", "L", "L"), ("R", "R", "R")),
           label_en="Kick button", label_es="Botón de patada",
           help_en="A 3DS button for the kick, which on a 3DS only the touch screen's panel does. With the Circle Pad Pro bindings, ZR is the usual one. Off: only the touch panel.",
           help_es="Un botón de la 3DS para la patada, que en la 3DS solo hace el panel de la pantalla táctil. Con los controles del Circle Pad Pro, ZR es el habitual. Desactivado: solo el panel táctil."),
    Option("target", "controls", "Extra buttons", "choice", "off",
           choices=(("off", "Off", "No"), ("ZL", "ZL", "ZL"), ("ZR", "ZR", "ZR"), ("L", "L", "L"), ("R", "R", "R")),
           label_en="Target camera button", label_es="Botón de cámara de objetivo",
           help_en="A 3DS button for the target camera panel: a press puts the camera on the monster, or goes through the large monsters and then off. ZL is the usual one. Off: only the touch panel.",
           help_es="Un botón de la 3DS para el panel de la cámara de objetivo: una pulsación fija la cámara en el monstruo, o pasa por los monstruos grandes y después se apaga. ZL es el habitual. Desactivado: solo el panel táctil."),
    Option("skip_end", "controls", "Extra buttons", "choice", "ZL+ZR",
           choices=(("ZL+ZR", "ZL+ZR", "ZL+ZR"), ("L+R", "L+R", "L+R"), ("L+R+X", "L+R+X", "L+R+X"), ("L+R+Y", "L+R+Y", "L+R+Y"), ("START+L+R", "START+L+R", "START+L+R"), ("off", "Off", "No"),),
           label_en="End the wait after a quest", label_es="Terminar la espera tras la misión",
           help_en="Buttons held together to end the wait before the return (the carving time) as if it had run out. Online it ends once every player with the port has held them. ZL and ZR are the Circle Pad Pro's: they are the keys or buttons you set in Controls.",
           help_es="Botones mantenidos juntos para terminar la espera antes de volver (el tiempo de despiece) como si se hubiera acabado. En línea termina cuando todos los jugadores con el port los han mantenido. ZL y ZR son los del Circle Pad Pro: son las teclas o botones que pones en Controles."),
    Option("skip_end_hold", "controls", "Extra buttons", "choice", "1.0", unit=" s",
           choices=(("0.5", "0.5 s", "0.5 s"), ("1.0", "1.0 s", "1.0 s"), ("1.5", "1.5 s", "1.5 s"), ("2.0", "2.0 s", "2.0 s"), ("3.0", "3.0 s", "3.0 s"), ("4.0", "4.0 s", "4.0 s"), ("5.0", "5.0 s", "5.0 s"),),
           label_en="Hold time to end the wait", label_es="Tiempo de pulsación para terminar la espera",
           help_en="How many seconds those buttons are held, so nobody ends the quest by accident.",
           help_es="Cuántos segundos se mantienen esos botones, para que nadie termine la misión sin querer."),
    Option("hud_scale", "mods", "Built-in mods", "int", "100", min=40, max=100, step=5, unit="%",
           label_en="Hunting HUD size", label_es="Tamaño del HUD de caza",
           help_en="Draws the hunting HUD smaller, from 100% (the game's own size) down to 40%. Each part keeps to its corner, so the smaller it is the closer to the edges. It only changes how the HUD is drawn, so it is fine online.",
           help_es="Dibuja el HUD de caza más pequeño, del 100% (el tamaño del juego) al 40%. Cada parte se queda en su esquina, así que cuanto más pequeño, más cerca de los bordes. Solo cambia cómo se dibuja el HUD, así que es seguro en línea."),
    Option("hud_parts", "mods", "Built-in mods", "bool", "off",
           label_en="Adjust each HUD part", label_es="Ajustar cada parte del HUD",
           help_en="Shows a size for each part of the hunting HUD (clock, bars, sharpness, party, map, target camera, item bar), on top of the general one. Switch it off to hide them again.",
           help_es="Muestra un tamaño para cada parte del HUD de caza (reloj, barras, filo, grupo, mapa, cámara de objetivo, barra de objetos), además del general. Apágalo para esconderlas otra vez."),
    Option("hud_scale_clock", "mods", "Built-in mods", "int", "100", min=40, max=100, step=5, unit="%",
           label_en="HUD: Clock", label_es="HUD: Reloj",
           help_en="This part's own size, on top of the general one (100% leaves it as it is).",
           help_es="El tamaño propio de esta parte, además del general (100% la deja como está).",
           depends_on="hud_parts=on"),
    Option("hud_scale_bars", "mods", "Built-in mods", "int", "100", min=40, max=100, step=5, unit="%",
           label_en="HUD: Health and stamina bars", label_es="HUD: Barras de vida y aguante",
           help_en="This part's own size, on top of the general one (100% leaves it as it is).",
           help_es="El tamaño propio de esta parte, además del general (100% la deja como está).",
           depends_on="hud_parts=on"),
    Option("hud_scale_sharpness", "mods", "Built-in mods", "int", "100", min=40, max=100, step=5, unit="%",
           label_en="HUD: Sharpness and weapon gauge", label_es="HUD: Filo y medidor del arma",
           help_en="This part's own size, on top of the general one (100% leaves it as it is).",
           help_es="El tamaño propio de esta parte, además del general (100% la deja como está).",
           depends_on="hud_parts=on"),
    Option("hud_scale_party", "mods", "Built-in mods", "int", "100", min=40, max=100, step=5, unit="%",
           label_en="HUD: Party name plates", label_es="HUD: Placas del grupo",
           help_en="This part's own size, on top of the general one (100% leaves it as it is).",
           help_es="El tamaño propio de esta parte, además del general (100% la deja como está).",
           depends_on="hud_parts=on"),
    Option("hud_scale_map", "mods", "Built-in mods", "int", "100", min=40, max=100, step=5, unit="%",
           label_en="HUD: Area map", label_es="HUD: Mapa del área",
           help_en="This part's own size, on top of the general one (100% leaves it as it is).",
           help_es="El tamaño propio de esta parte, además del general (100% la deja como está).",
           depends_on="hud_parts=on"),
    Option("hud_scale_target", "mods", "Built-in mods", "int", "100", min=40, max=100, step=5, unit="%",
           label_en="HUD: Target camera", label_es="HUD: Cámara de objetivo",
           help_en="This part's own size, on top of the general one (100% leaves it as it is).",
           help_es="El tamaño propio de esta parte, además del general (100% la deja como está).",
           depends_on="hud_parts=on"),
    Option("hud_scale_items", "mods", "Built-in mods", "int", "100", min=40, max=100, step=5, unit="%",
           label_en="HUD: Item bar", label_es="HUD: Barra de objetos",
           help_en="This part's own size, on top of the general one (100% leaves it as it is).",
           help_es="El tamaño propio de esta parte, además del general (100% la deja como está).",
           depends_on="hud_parts=on"),
    Option("hd_textures", "graphics", "Rendering", "bool", "on", restart=True, in_game=False,
           label_en="HD textures", label_es="Texturas HD",
           help_en="Loads HD texture packs (Azahar or Citra format, or the converted .mh4hdpk file) from the textures folder when the game starts.",
           help_es="Carga los paquetes de texturas HD (formato Azahar o Citra, o el archivo convertido .mh4hdpk) de la carpeta de texturas al abrir el juego."),
    Option("hd_texture_max", "graphics", "Rendering", "choice", "off", restart=True,
           choices=(("off", "No limit", "Sin límite"), ("4096", "4096 px", "4096 px"), ("2048", "2048 px", "2048 px"),
                    ("1024", "1024 px", "1024 px"), ("512", "512 px", "512 px")),
           label_en="HD texture size limit", label_es="Límite de tamaño de texturas HD",
           help_en="A texture of an HD pack larger than this (on its longer side) is reduced before it is loaded: it takes less video memory and loads faster, with less detail. Off keeps them as they are.",
           help_es="Una textura de un pack HD mayor que esto (en su lado más largo) se reduce antes de cargarla: usa menos memoria de vídeo y carga más rápido, con menos detalle. Sin límite las deja como están."),
    Option("gpu_thread", "graphics", "Rendering", "bool", "off", restart=True, in_game=False,
           label_en="Dedicated GPU thread", label_es="Hilo dedicado para la GPU",
           help_en="Sends the drawing work to a separate thread, so the game logic does not wait for the graphics driver. It can raise the frame rate on a busy scene; turn it off if you see glitches.",
           help_es="Envía el trabajo de dibujo a un hilo aparte, para que la lógica del juego no espere al driver gráfico. Puede subir los fotogramas en escenas pesadas; apágalo si ves fallos visuales."),
    Option("shader_fallback", "graphics", "Rendering", "bool", "off", restart=True, in_game=False,
           label_en="Fewer shader stutters (experimental)", label_es="Menos parones por shaders (experimental)",
           help_en="While the game prepares a new shader, it draws with a general one when it is ready, so the picture stays still less often. The frame rate can drop for a moment and the graphics card works harder. It does not remove every stutter; turn it off if you see glitches.",
           help_es="Mientras el juego prepara un shader nuevo, dibuja con uno general si ya está listo, así la imagen se queda pegada menos veces. Los fotogramas pueden bajar un momento y la tarjeta gráfica trabaja más. No quita todos los parones; apágalo si ves fallos visuales."),
    Option("fps", "game", "Frame rate", "choice", "default", unit="Hz",
           choices=(_DEFAULT, ("display", "Display", "Tu pantalla"),
                    *((str(n), str(n), str(n)) for n in (30, 60, 120, 180, 240, 300, 360)),
                    ("unlimited", "Unlimited", "Sin límite")),
           label_en="Frame rate limit", label_es="Límite de fotogramas",
           help_en="Caps the game's frame rate. Default is the game's own, Display matches your screen, Unlimited uses all the CPU.",
           help_es="Limita los fotogramas del juego. Predeterminado es el del juego, Tu pantalla iguala su frecuencia, Sin límite usa toda la CPU."),
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
           help_en="Click the box and press the keyboard key that taps the Skip button of a cutscene. The key keeps doing what it does in the game too.",
           help_es="Haz clic en el cuadro y pulsa la tecla del teclado que toca Omitir en una cinemática. La tecla sigue haciendo lo suyo en el juego."),
    Option("skip_pad", "controls", "Skip a cutscene", "padbutton", "",
           label_en="Skip a cutscene (controller)", label_es="Saltar cinemática (mando)",
           help_en="The controller button that taps the Skip button of a cutscene. It keeps doing what it does in the game too.",
           help_es="El botón del mando que toca Omitir en una cinemática. Sigue haciendo lo suyo en el juego."),
    Option("map_key", "controls", "Open the map", "key", "M",
           label_en="Open the world map (key)", label_es="Abrir el mapa (tecla)",
           help_en="Click the box and press the keyboard key that opens the world map on the main screen, to travel from village to village. It only works in a village, out of a quest.",
           help_es="Haz clic en el cuadro y pulsa la tecla del teclado que abre el mapa del mundo en la pantalla principal, para viajar de aldea en aldea. Solo funciona en una aldea, fuera de misión."),
    Option("map_pad", "controls", "Open the map", "padbutton", "b6",
           label_en="Open the world map (controller)", label_es="Abrir el mapa (mando)",
           help_en="The controller button that opens the world map on the main screen. It only works in a village, out of a quest. It keeps doing what it does in the game too.",
           help_es="El botón del mando que abre el mapa del mundo en la pantalla principal. Solo funciona en una aldea, fuera de misión. Sigue haciendo lo suyo en el juego."),
    Option("circle_pad_pro", "controls", "Input devices", "bool", "on", restart=True, in_game=False,
           label_en="Circle Pad Pro", label_es="Circle Pad Pro",
           help_en="The Circle Pad Pro is the 3DS's second analog stick, which the game uses for the camera. Keep it on to move the camera with your controller's right stick.",
           help_es="El Circle Pad Pro es el segundo stick analógico de la 3DS, que el juego usa para la cámara. Déjalo activo para mover la cámara con el stick derecho del mando."),
    Option("mouse_camera", "controls", "Input devices", "bool", "off", in_game=False,
           label_en="Mouse camera", label_es="Cámara con ratón",
           help_en="Uses the mouse for the camera. Hold Left Alt to use the stylus, or press F3 to toggle the mouse mode.",
           help_es="Usa el ratón para la cámara. Mantén pulsado Alt izquierdo para usar el lápiz táctil o presiona F3 para cambiar de modo."),
    Option("hid_pads", "controls", "Input devices", "bool", "on", restart=True, in_game=False,
           label_en="HID controllers", label_es="Mandos HID",
           help_en="Reads PlayStation and other controllers directly (not only Xbox-style ones), with their lights, triggers and sticks. Turn it off if a controller is detected twice.",
           help_es="Lee mandos de PlayStation y otros directamente (no solo los tipo Xbox). Apágalo si un mando se detecta dos veces."),
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
           label_en="In-game menu language", label_es="Idioma del menú del juego",
           help_en="The language of the menu inside the game (F1) and of the monster names on the life bar. Auto follows Windows. The launcher's own language is the one in its gear menu, top right.",
           help_es="El idioma del menú dentro del juego (F1) y de los nombres de monstruo en la barra de vida. Auto sigue el de Windows. El idioma del launcher es el del menú del engranaje, arriba a la derecha."),
    Option("discord_presence", "system", "Integration", "bool", "on", restart=True, in_game=False,
           label_en="Discord Rich Presence", label_es="Presencia enriquecida de Discord",
           help_en="Shows in your Discord profile that you are playing, and for how long.",
           help_es="Muestra en tu perfil de Discord que estás jugando y por cuánto tiempo."),
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
        if value.lower() in ("f1", "f2", "f3", "f8", "f11", "escape", "esc"):  # the keys the program itself uses while the game runs
            return False, "That key is used by the program (F1 menu, F2 touch popup, F3 mouse camera, F8 log mark, F11 full screen, Esc)."
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
