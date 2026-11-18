# Caravan4URecomp

**Monster Hunter 4 Ultimate on Windows, running natively through Vulkan.**

Caravan4URecomp is my Windows port of [4u-native](https://github.com/khorne-hub/4u-native) by khorne-hub. It is a **modified
version**: it is not the official 4u-native, and it is not made or endorsed by khorne-hub. All the hard work (the
recompiler, the runtime and the original app) is theirs and CompaLarios18's, and I'm very grateful for it. What I added is
a Windows shell around that runtime.

[Leer en español](#español)

**This project includes no game code, game files, ROMs or keys. You need your own copy of the game.**

## Status

I'd rather be upfront about what I have actually tested.

**Working (tested on Windows 11, NVIDIA RTX 4070):**
- The game boots and runs at 60 fps with the USA version, v1.3.0.
- Character creation, a full quest and a complete hunt, without crashes.
- Keyboard, mouse (as the stylus of the touch screen) and controller, including typing a hunter's name with a controller.
- Windowed and fullscreen (F11 or Alt+Enter), resizing, and a 3x internal resolution by default.
- The top screen fills the window; the touch screen appears as a small popup over it only when the game is waiting for you
  there (the save selection and character creation, the game's keyboard, a few town pages), and the mouse works on it.
- An in-game settings menu (see below).
- Network rooms that speak the same protocol as Azahar's: I joined a real public room from the in-game menu while the game was running, and I played Local Play in a Gathering Hall with another player.
- Scales up to 8x the 3DS's resolution, and a frame counter that shows the frames you actually see.

**Not tested yet:**
- The new Controls, Saves and Mods pages: I tested them with the keyboard and with a copy of my saves; remapping a controller button, restoring a backup and dropping a mod have not been tried by other people yet.
- The one-click **Host a room** and **Join with the copied address** buttons of the Online page (new in 0.2.0), and the touch screen staying up while you are in a room.
- The mouse as the camera (**F3**, or `mouse_camera = on` in the ini).
- Resolutions of 9x and 10x: the picture still comes out black there.
- Other GPUs and drivers, Windows 10, and building with MSVC (I build with clang from llvm-mingw).
- Windows and Android crossplay.

## Getting it

The releases are on the [Releases](https://github.com/G30vamv/Caravan4URecomp/releases) page; the latest is **0.3.0**, tagged `windows-v0.3.0`. This port has its
own version numbers, following [semantic versioning](https://semver.org) (while the number starts with 0, anything can still
change), and they are tagged `windows-vX.Y.Z` so they don't mix with the older tags in this repository (`v0.1.0` to `v1.0`),
which are 4u-native's own. Each download has its SHA-256 next to it. The program is not code-signed, so Windows may show a
SmartScreen warning: "More info", then "Run anyway". You can also build it yourself (see [Building](#building)).

## What you need

- Windows 10 or 11, 64-bit, and a GPU with Vulkan 1.2 or newer.
- About 1 GB of free space while the game is prepared (afterwards it takes about 350 MB: the game's code library and the part of the compiler that was downloaded).
- **Your own decrypted copy of Monster Hunter 4 Ultimate (USA):** the base game and the **v1.3.0 update**.

## First start

Open `Caravan4URecomp.exe`. The first time it asks for the two game files and builds the game's code library for your PC,
which takes a few minutes and downloads only the part of a compiler that it needs (about 50 MB; each file is checked against its SHA-256). After that the game starts
straight away. `--setup` repeats this step if you change files. What it builds is made from your game: keep it for
yourself and don't share it.

## Controls

Keyboard: F = A, Space = B, V = X, E = Y, Enter = Start, Backspace = Select, arrows = D-pad, W A S D = left stick,
O L K ; = right stick, Q = ZL, C = ZR, left Ctrl = L, left Shift = R, Tab = skip a cutscene. The mouse is the stylus on
the bottom screen. A controller works as it is.

### In-game menu

Press **F1** or **L3 + R3** on a controller. It works with the keyboard, the mouse (click, drag the sliders, wheel,
right click to go back) and the controller (D-pad or left stick, A and B; LB and RB, or Q and E, change the page). The
design comes from the menu of my MH3U port, with this game's colours, in the Cinzel font (SIL Open Font License).

- **Graphics:** fullscreen, VSync, screen layout, picture shape (the 3DS's own 5:3 with bars on a wide screen, stretched to fill the window, or a fixed ratio), internal resolution (auto, or 1x to 8x the 3DS's; needs a restart), FPS counter.
- **Game:** frame rate limit (the game's own, your display's rate, 30, 60, 120, 180, 240, 300, 360 or unlimited), steady
  frame rate, particle effects.
- **Audio:** volume.
- **Controls:** every 3DS button with its key and its controller button. Press Enter on one, then the key or the controller's button or trigger you want (Backspace puts it back). The sticks and F1, F2, F3 and F11 stay as they are.
- **Saves:** backups of your saves. One is made by hand, or automatically each time the game starts if the saves changed (the newest 5 automatic ones are kept). Enter twice on a backup puts it back the next time the game starts, after a backup of what was there.
- **Mods:** drop a mod's folder or .zip on the window to install it, turn mods on and off and set their order (they apply the next time the game starts). Mods that change the game's files work; mods that change its code are installed turned off, because the game's library is built without them.
- **System:** the language of the menu (English or Spanish; "auto" follows Windows), resume and quit.
- **Online:** nickname, host a room, join with the copied address, public rooms, join by address, leave.

The default screen layout is the **popup** one described above: **F2** shows or hides the touch screen by hand, and the
menu lets you choose when it shows (auto, always, hidden), its size and its corner. The classic layouts (stacked, overlay,
single, side) are still there. Every change is saved to `Caravan4URecomp.ini`, next to the program, as soon as you make it, so it survives closing the game
normally or not. A few settings (the internal resolution, HD textures, and going back to "default" in the game options) only
apply the next time the game starts: the menu saves them, says so, and lists them until you close the game; it never
restarts anything by itself. **Graphics > Open the HD textures folder** opens the folder for texture packs.

## Frame rate and resolution

- **Frame rate:** the limit goes up to 360 fps. The counter shows the frames that reach your screen, not the ones the game
  makes: with a heavy internal resolution the two can differ, and then it feels choppy even if the game runs fast. With VSync
  on you get at most your display's refresh rate, so "display" in the limit setting is the natural choice. If it feels
  uneven at a high refresh rate, lower the internal resolution (on my screen 3x and 5x showed almost no lost frames at 144 fps, and 7x lost about 15 %). Each frame the game shows is read back from the GPU, so the highest rate the game can show depends on the internal resolution: about 580 fps at 3x, 160 at 6x and 97 at 8x. A higher limit, including unlimited, is held at that. Before 0.3.0, unlimited (or a very high limit) with a high internal resolution could leave the picture black.
- **Internal resolution (1x to 8x):** everything the GPU draws (the 3D scene, the HUD and the text) is rendered at that multiple
  of the 3DS's resolution; "auto" picks the one closest to your window. The game's own textures stay at their original
  resolution, so edges get sharper but not more detailed. HD texture packs made for Azahar and Citra (PNG, their folder
  layout and hashes) are supported by the runtime: put them under `game\user\load\textures`. At 9x and 10x the picture
  is still black on my card (8x works since 0.2.0), so the menu stops at 8x.
- **Picture shape:** the 3DS's top screen is 5:3, so on a 16:9 screen there are black bars at the sides. **Graphics > Picture
  shape > stretch** fills the whole window (the picture is stretched a little); the fixed ratios are there too.

## Playing online

The rooms are the same kind Azahar uses, so players on this port, on Azahar and on 4u-native for Android can share a
room. Everyone needs MH4U USA v1.3.0.

1. Open the menu, **Online**, and choose a nickname.
2. To **host**: choose **Host a room**. The program looks for a Radmin VPN, Hamachi, ZeroTier or Tailscale address on your PC
   (with one of those there is no router to open) and otherwise for your local network's, and **copies the address with
   its port** (for example `26.1.2.3:24872`) to the clipboard. Send it to your friends. Without a VPN you need the UDP port (24872 by default) open on your router.
3. To **join** a friend: copy the address they sent and choose **Join with the copied address**. Or **Public rooms** lists the
   rooms of the community lobby (a `*` marks the ones set up for MH4U), or type a `HOST:PORT` address by hand. If you write no port, 24872 is used.
4. Being in the room is not enough: in the game, go to the **Gathering Hall** and choose **Local Play**. While you are in a
   room the touch screen stays up in towns (`online_touch = off` in the ini turns that off).

From the command line: `--list-rooms [URL]`, `--join HOST:PORT`, `--host PORT`, `--nick NAME`, `--room-password TEXT`.
The lobby is read over plain HTTP, like Azahar does. Hosted rooms are not announced to the lobby, so people find your room
by its address, not in Azahar's list.

## Building

You need CMake, Ninja, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT, x86_64) and Python 3.10 or newer. In a
PowerShell window:

```
powershell -ExecutionPolicy Bypass -File windows\build.ps1
powershell -ExecutionPolicy Bypass -File windows\package.ps1
```

`build.ps1` downloads SDL3 and builds the program; `package.ps1` makes the zip. The game's code library is built on each
player's PC from their own files, and it is never part of this repository.

## What I changed in 4u-native

- New: the `windows/` folder (the Windows shell, the in-game menu, the build and packaging scripts).
- New: `mh4n/native/src/applets/extrapad.{h,cpp}`, a minimal Extrapad applet so the Circle Pad Pro option in the game's
  settings doesn't close the game.
- Changed: `mh4n/port/src/hooks.cpp` and `hooks.h` (one counter, `TouchScreenObjects`, of the layout objects the game draws on
  the touch screen, which the shell uses to know when the touch screen has something on it).
- Changed: `mh4n/native/src/gpu/native/native_gpu.cpp` (the renderer uses the Vulkan device the window shell already has, and the
  screen is read out without a one-pixel shift that Azahar's software renderer has and that drew a wrong line along the top
  of the picture; the same fix is in `gpu_capture/image.cpp` and `display_mem/display_mem.cpp`, and in the two tests that
  expected the old index),
- Changed: `mh4n/port/src/onescreen_table.h` (a generated table, edited by hand: the Gathering Hall's player list stays on
  the touch screen instead of covering the top one),
  `mh4n/native/src/services/apt/applet_manager.cpp` and `mh4n/native/src/applets/CMakeLists.txt`.

Every changed file says so in its header. The Android app is untouched.

## Credits

- The recompiler and the native runtime: [MH4UltimateDC](https://github.com/CompaLarios18/MH4UltimateDC) by CompaLarios18
  and its contributors.
- 4u-native, the Android app this port is based on: [khorne-hub](https://github.com/khorne-hub/4u-native).
- Parts of the runtime come from [Azahar](https://github.com/azahar-emu/azahar) and Citra.
- The libraries used, with their licenses, are listed in `THIRD_PARTY_NOTICES` and `windows/THIRD_PARTY_NOTICES_WINDOWS`.
- The full credits of 4u-native are in its [README](../README.md), which is kept as it was.

## License

Free software under the [GNU General Public License, version 3](../LICENSE) or (at your option) any later version. The code
by khorne-hub carries [additional terms](../ADDITIONAL-TERMS), which still apply: keep the author's credits, mark a changed
version as changed and don't present it as the official one, and give the license and the complete source code with
every copy of the program. New code of this port: Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors.

This project is not affiliated with, endorsed by or sponsored by the Great C or the Great N. Monster Hunter is a trademark
of the Great C. The 3DS is a trademark of the Great N. It contains no game code, game data, ROMs or keys. Use it only with
a copy of the game you own.

---

## Español

**Monster Hunter 4 Ultimate en Windows, nativo y con Vulkan.**

Caravan4URecomp es mi port para Windows de [4u-native](https://github.com/khorne-hub/4u-native), de khorne-hub. Es una
**versión modificada**: no es la oficial ni está hecha ni respaldada por khorne-hub. El trabajo difícil (el recompilador,
el runtime y la app original) es de ellos y de CompaLarios18, y se lo agradezco mucho. Yo añadí un shell para Windows
alrededor de ese runtime.

**No incluye el juego.** Necesitas tu propia copia descifrada de Monster Hunter 4 Ultimate (USA) y su update v1.3.0.

**Estado:** probado en Windows 11 con una RTX 4070: el juego corre a 60 fps, he completado una misión y una cacería,
funciona con teclado, ratón y mando, y entré a una sala pública real de Azahar desde el menú del juego. Todavía no he
probado Local Play con otras personas, otras GPU, Windows 10 ni MSVC.

**Cómo se usa:** la última versión es la 0.3.0, en la página de Releases (con su suma SHA-256). Abre `Caravan4URecomp.exe`: la
primera vez pide los dos archivos del juego y construye la librería de código para tu PC. El menú del juego se abre con
**F1** o **L3 + R3**, y tiene gráficos, juego, audio y online. Online usa las mismas salas que Azahar: menú → Online →
**Alojar una sala** (copia al portapapeles la dirección con puerto de tu Radmin VPN, Hamachi, ZeroTier o Tailscale, o de tu red local, para mandarla a tus amigos) o **Unirse con la dirección copiada**, salas públicas (el * marca las de MH4U) o una dirección `HOST:PORT`; y luego, en el juego, Gathering Hall → Local Play.

Para compilarlo: `windows\build.ps1` y `windows\package.ps1` (CMake, Ninja, llvm-mingw y Python). Los créditos y la licencia
están arriba; los términos adicionales de khorne-hub siguen aplicando.
