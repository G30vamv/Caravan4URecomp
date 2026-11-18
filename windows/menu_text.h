// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// The words of the in-game menu in Spanish. The menu is written in English; `Tr` gives the Spanish of a text when the language
// is Spanish (setting `language` = es, or auto with Windows in Spanish) and the text itself otherwise or when it has no entry.
#pragma once

#include <string>
#include <unordered_map>

namespace ingame {

inline bool g_spanish = false;  // set by the menu before it builds a page

inline std::string Tr(const std::string& en) {
    if (!g_spanish) return en;
    static const std::unordered_map<std::string, std::string> kEs = {
        // tabs and titles
        {"Graphics", "Gráficos"}, {"Game", "Juego"}, {"Audio", "Audio"}, {"Online", "En línea"}, {"System", "Sistema"},
        {"Public rooms", "Salas públicas"}, {"Room password", "Contraseña de la sala"},
        // rows
        {"Fullscreen", "Pantalla completa"}, {"VSync", "VSync"}, {"Screen layout", "Disposición de pantallas"}, {"Picture shape", "Forma de la imagen"},
        {"Default keeps the 3DS's own 5:3 shape (black bars at the sides on a wide screen). Stretch fills the whole window. The ratios draw the picture in that shape.", "Default mantiene la forma 5:3 de la 3DS (barras negras a los lados en una pantalla ancha). Stretch llena toda la ventana. Las proporciones dibujan la imagen con esa forma."},
        {"Touch screen shows", "Pantalla táctil"}, {"Touch popup size", "Tamaño del recuadro táctil"},
        {"Touch popup corner", "Esquina del recuadro táctil"}, {"Internal resolution", "Resolución interna"},
        {"FPS counter", "Contador de FPS"}, {"Open the HD textures folder", "Abrir la carpeta de texturas HD"},
        {"Frame rate limit", "Límite de fotogramas"}, {"Steady frame rate", "Fotogramas estables"},
        {"Particle effects", "Efectos de partículas"}, {"Volume", "Volumen"}, {"Language", "Idioma"},
        {"Nickname", "Apodo"}, {"Public rooms >", "Salas públicas >"}, {"Address HOST:PORT", "Dirección HOST:PUERTO"},
        {"Join this address", "Unirse a esta dirección"}, {"Host port (UDP)", "Puerto para alojar (UDP)"},
        {"Host password", "Contraseña de tu sala"}, {"Host a room", "Alojar una sala"}, {"Close the room I host", "Cerrar mi sala"},
        {"Leave the room", "Salir de la sala"}, {"Resume the game", "Volver al juego"}, {"Quit the game", "Cerrar el juego"},
        {"Refresh the list", "Actualizar la lista"}, {"Refreshing...", "Actualizando..."}, {"Password", "Contraseña"},
        {"Join", "Unirse"}, {"HD textures", "texturas HD"},
        // buttons
        {"Open", "Abrir"}, {"Host", "Alojar"}, {"Close", "Cerrar"}, {"Leave", "Salir"}, {"Refresh", "Actualizar"},
        {"Resume", "Volver"}, {"Quit", "Cerrar"}, {"Accept", "Aceptar"},
        // values
        {"On", "Sí"}, {"Off", "No"}, {"default", "Predeterminado"}, {"display", "Tu pantalla"}, {"unlimited", "Sin límite"},
        {"auto", "Auto"}, {"Auto", "Auto"}, {"always", "Siempre"}, {"hidden", "Oculta"}, {"popup", "Recuadro"}, {"stacked", "Apiladas"},
        {"overlay", "Superpuestas"}, {"single", "Una sola"}, {"side", "Lado a lado"}, {"bottom-right", "Abajo a la derecha"},
        {"bottom-left", "Abajo a la izquierda"}, {"top-right", "Arriba a la derecha"}, {"top-left", "Arriba a la izquierda"},
        {"en", "English"}, {"es", "Español"}, {"password", "contraseña"}, {"members", "miembros"},
        // keys of the bottom line
        {"Accept  (Enter)", "Aceptar  (Enter)"}, {"Back  (Esc)", "Volver  (Esc)"}, {"Pages  (Q / E)", "Páginas  (Q / E)"},
        {"Close  (F1)", "Cerrar  (F1)"},
        // lines of text
        {"Not in a room.", "No estás en una sala."}, {"In room: ", "En la sala: "}, {"Joining ", "Uniéndose a "},
        {" (waiting for the game)", " (esperando al juego)"}, {"Not in a room: ", "No estás en una sala: "},
        {"Hosting on UDP port ", "Alojando en el puerto UDP "}, {" (open it on your router)", " (ábrelo en tu router)"},
        {"Then, in the game: Gathering Hall, Local Play.", "Después, en el juego: Gathering Hall, Local Play."},
        {"Every change is saved as soon as you make it.", "Cada cambio se guarda en cuanto lo haces."},
        {"Open this menu with F1, or L3 + R3 on a controller.", "Abre este menú con F1, o con L3 + R3 en un mando."},
        {"Loading the list of rooms...", "Cargando la lista de salas..."},
        {"Rooms for MH4U come first. You need MH4U USA 1.3.0.", "Las salas de MH4U van primero. Necesitas MH4U USA 1.3.0."},
        {"Room: ", "Sala: "},
        {"Waiting for a restart of the game: ", "Esperan a que reinicies el juego: "},
        // controls, saves and mods
        {"Controls", "Controles"}, {"Saves", "Partidas"}, {"Mods", "Mods"},
        {"Enter on a button, then press the key or the controller's button you want for it.", "Enter sobre un botón y luego pulsa la tecla o el botón del mando que quieras para él."},
        {"Now press a key or a controller button.  Esc or the right mouse button cancels, Backspace puts it back.", "Ahora pulsa una tecla o un botón del mando.  Esc o el botón derecho del ratón cancela, Retroceso lo deja como estaba."},
        {"Press a key or a button...", "Pulsa una tecla o un botón..."},
        {"Up (D-pad)", "Arriba (cruceta)"}, {"Down (D-pad)", "Abajo (cruceta)"}, {"Left (D-pad)", "Izquierda (cruceta)"}, {"Right (D-pad)", "Derecha (cruceta)"},
        {"The key and the controller's button of this 3DS button. A keyboard key you press replaces the key, a controller button or trigger replaces the controller's.", "La tecla y el botón del mando de este botón de la 3DS. Una tecla que pulses cambia la tecla; un botón o gatillo del mando cambia el del mando."},
        {"Put every control back as it was", "Dejar todos los controles como estaban"}, {"Reset", "Restablecer"},
        {"The sticks: W A S D and the left stick (circle pad), O L K ; and the right stick (C-stick).", "Los sticks: W A S D y el stick izquierdo (palanca deslizante), O L K ; y el stick derecho (C-stick)."},
        {"F1 (the menu), F2, F3 and F11 are the program's own keys.", "F1 (el menú), F2, F3 y F11 son teclas propias del programa."},
        {"Saved: ", "Guardado: "}, {"Saved.", "Guardado."}, {"Put back as it was.", "Vuelto a como estaba."}, {"Every control is back as it was.", "Todos los controles están como estaban."},
        {"Your saves: ", "Tus partidas: "}, {"Backed up as ", "Copia hecha: "}, {"files", "archivos"},
        {"There are no saves yet. They appear when the game saves.", "Todavía no hay guardados. Aparecen cuando el juego guarda."},
        {"There are no saves yet.", "Todavía no hay guardados."},
        {"A restore is waiting: ", "Hay una restauración pendiente: "},
        {"It is done the next time the game starts (the saves now are backed up first).", "Se hace la próxima vez que abras el juego (antes se copian los guardados actuales)."},
        {"Cancel the restore", "Cancelar la restauración"}, {"The restore was cancelled.", "Se canceló la restauración."},
        {"Back up my saves now", "Copiar mis guardados ahora"}, {"Back up", "Copiar"},
        {"Back up automatically at start", "Copia automática al abrir el juego"},
        {"Open the saves folder", "Abrir la carpeta de guardados"}, {"Open the backups folder", "Abrir la carpeta de copias"},
        {"No backups yet.", "Todavía no hay copias."},
        {"Backups, newest first. Enter restores one; Backspace deletes it.", "Copias, las más nuevas primero. Enter restaura una; Retroceso la borra."},
        {"before-restore", "antes de restaurar"}, {"mine", "mía"},
        {"Press Enter again to put this backup back the next time the game starts.", "Pulsa Enter otra vez para restaurar esta copia la próxima vez que abras el juego."},
        {"Press Backspace again to delete this backup.", "Pulsa Retroceso otra vez para borrar esta copia."},
        {"It will be put back the next time the game starts.", "Se restaurará la próxima vez que abras el juego."},
        {"That backup is not there.", "Esa copia no existe."}, {"The backup was deleted.", "Se borró la copia."},
        {"The backup could not be deleted.", "No se pudo borrar la copia."}, {"The folder is open", "La carpeta está abierta"},
        {"A copy of your saves. Enter twice puts it back the next time the game starts (the saves now are backed up first). Backspace twice deletes it.", "Una copia de tus guardados. Enter dos veces la restaura la próxima vez que abras el juego (antes se copian los actuales). Retroceso dos veces la borra."},
        {"Backs up your saves each time the game starts, if they changed since the last backup. The newest 5 automatic backups are kept; yours are never deleted.", "Copia tus guardados cada vez que abres el juego, si cambiaron desde la última copia. Se guardan las 5 automáticas más nuevas; las tuyas nunca se borran."},
        {"Copies your saves to the backups folder.", "Copia tus guardados a la carpeta de copias."},
        {"Mods change the game's files. A change applies the next time the game starts.", "Los mods cambian los archivos del juego. Un cambio se aplica la próxima vez que abras el juego."},
        {"No mods yet. Drop a mod's folder or .zip on this window, or open the folder and copy it there.", "Todavía no hay mods. Suelta la carpeta o el .zip de un mod en esta ventana, o abre la carpeta y cópialo ahí."},
        {" (code)", " (código)"}, {": move up", ": subir"}, {": move down", ": bajar"}, {"Up", "Subir"}, {"Down", "Bajar"},
        {"Open the mods folder", "Abrir la carpeta de mods"},
        {"This mod changes the game's code: it needs the game's library built with it, which this program does not do.", "Este mod cambia el código del juego: haría falta construir la librería del juego con él, y este programa no lo hace."},
        {"A code mod: it changes the game's code, so the game's library would have to be built with it. It can be turned off here, not on.", "Un mod de código: cambia el código del juego, así que la librería del juego tendría que construirse con él. Aquí se puede apagar, no encender."},
        {"Turns this mod on or off. Where two mods change the same file, the one lower in the list wins.", "Enciende o apaga este mod. Si dos mods cambian el mismo archivo, gana el que está más abajo en la lista."},
        // what the menu says
        {"Saved. This change applies the next time you start the game.", "Guardado. Este cambio se aplica la próxima vez que abras el juego."},
        {"Type the address as HOST:PORT.", "Escribe la dirección como HOST:PUERTO."}, {"Room closed.", "Sala cerrada."},
        {"The port must be 1 to 65535.", "El puerto debe ir de 1 a 65535."}, {"Left the room.", "Saliste de la sala."},
        {"Nickname: 4 to 20 letters, digits, spaces, dots, underscores or dashes.", "Apodo: de 4 a 20 letras, dígitos, espacios, puntos, guiones o guiones bajos."},
        {"Hosting: others join your public IP address, port ", "Alojando: los demás entran a tu IP pública, puerto "},
        {"Not available", "No disponible"}, {"The textures folder is open", "La carpeta de texturas está abierta"},
        {"No user folder is set", "No hay carpeta de usuario"}, {". Restart the game to load the packs.", ". Reinicia el juego para cargar los packs."},
        // the first start
        {"First start", "Primer arranque"}, {"Preparing your game", "Preparando tu juego"},
        {"Choose a file, or drop it here", "Elige un archivo, o suéltalo aquí"},
        {"Caravan4URecomp does not include the game. Choose YOUR OWN decrypted copy of Monster Hunter 4 Ultimate (USA): the base game and its v1.3.0 update. This is done once and takes a few minutes; the first time it also downloads the part of a compiler that it needs, about 50 MB (it needs Internet). What it builds stays on this PC: please don't share it.",
         "Caravan4URecomp no incluye el juego. Elige TU PROPIA copia descifrada de Monster Hunter 4 Ultimate (USA): el juego base y su actualización v1.3.0. Se hace una sola vez y tarda unos minutos; la primera vez también descarga la parte de un compilador que necesita, unos 50 MB (hace falta Internet). Lo que construye se queda en este PC: por favor, no lo compartas."},
        {"The base game (.cia, .3ds or .cci)", "El juego base (.cia, .3ds o .cci)"}, {"The v1.3.0 update (.cia)", "La actualización v1.3.0 (.cia)"},
        {"You can also drag the files onto this window.", "También puedes arrastrar los archivos a esta ventana."},
        {"Browse", "Buscar"}, {"Start", "Empezar"}, {"Quit", "Salir"}, {"Cancel", "Cancelar"}, {"Try again", "Reintentar"},
        {"You can leave this window open: it keeps working.", "Puedes dejar esta ventana abierta: sigue trabajando."},
        {"Ready! Starting the game...", "¡Listo! Abriendo el juego..."}, {"Something went wrong", "Algo salió mal"},
        {"The game could not be prepared.", "No se pudo preparar el juego."},
        {"Starting...", "Empezando..."}, {"Downloading the compiler", "Descargando el compilador"},
        {"Unpacking the compiler", "Descomprimiendo el compilador"}, {"Reading your game files", "Leyendo los archivos de tu juego"},
        {"Translating the game's code", "Traduciendo el código del juego"},
        {"Compiling the game's code for this PC", "Compilando el código del juego para este PC"},
        {"Linking the code library", "Armando la librería de código"}, {"Preparing the game folder", "Preparando la carpeta del juego"},
        {"Done.", "Listo."}, {"Less than a minute left.", "Falta menos de un minuto."}, {"About ", "Faltan unos "},
        {" minutes left.", " minutos."}, {"PowerShell did not start.", "PowerShell no arrancó."},
        {"The script could not be started.", "No se pudo iniciar el script."},
        // help of each row
        {"Borderless fullscreen on the display the window is on. F11 or Alt+Enter does the same.", "Pantalla completa sin bordes en la pantalla donde está la ventana. F11 o Alt+Enter hacen lo mismo."},
        {"Shows each frame when your display refreshes. Off lets the game run above your display's rate, and the picture may tear.", "Muestra cada fotograma cuando se refresca tu pantalla. Apagado deja que el juego supere su frecuencia, y la imagen puede cortarse."},
        {"Popup: the top screen fills the window and the touch screen pops up over it when the game needs it. The others show both screens all the time.", "Recuadro: la pantalla de arriba llena la ventana y la táctil aparece encima cuando el juego la necesita. Las demás muestran las dos pantallas siempre."},
        {"When the touch screen's popup appears: when the game needs it (auto), always, or never. F2 shows or hides it by hand.", "Cuándo aparece el recuadro de la pantalla táctil: cuando el juego la necesita (auto), siempre o nunca. F2 lo muestra u oculta a mano."},
        {"The height of the touch screen's popup, as a percentage of the window's height.", "La altura del recuadro de la pantalla táctil, como porcentaje de la altura de la ventana."},
        {"The corner of the window where the touch screen's popup sits.", "La esquina de la ventana donde se pone el recuadro de la pantalla táctil."},
        {"How sharp the 3D scene, the HUD and the text are drawn: a multiple of the 3DS top screen's 400 x 240. Auto picks the one closest to your window.", "Qué tan nítidos se dibujan la escena 3D, el HUD y el texto: un múltiplo de la resolución de la 3DS. Auto elige el más cercano a tu ventana."},
        {"Shows the game's frame rate in the top-left corner.", "Muestra los fotogramas por segundo del juego en la esquina superior izquierda."},
        {"Opens the folder for HD texture packs (Azahar or Citra format). They are read when the game starts.", "Abre la carpeta de los packs de texturas HD (formato Azahar o Citra). Se leen al abrir el juego."},
        {"Caps the game's frame rate. Default is the game's own, Display matches your screen, Unlimited uses all the CPU.", "Limita los fotogramas del juego. Predeterminado es el del juego, Tu pantalla iguala su frecuencia, Sin límite usa toda la CPU."},
        {"Keeps the frame rate even instead of letting it vary. Default leaves the game's own setting.", "Mantiene los fotogramas parejos en vez de dejarlos variar. Predeterminado deja el ajuste del juego."},
        {"Turns the game's particle effects on or off. Default leaves the game's own setting.", "Activa o desactiva los efectos de partículas del juego. Predeterminado deja el ajuste del juego."},
        {"The volume of the game.", "El volumen del juego."},
        {"The language of this menu. Auto follows Windows.", "El idioma de este menú. Auto sigue el de Windows."},
        {"Your name in network rooms: 4 to 20 letters, digits, spaces, dots, underscores or dashes.", "Tu nombre en las salas en línea: de 4 a 20 letras, dígitos, espacios, puntos, guiones o guiones bajos."},
        {"Lists the rooms of the community lobby (the same rooms Azahar uses). Everyone needs Monster Hunter 4 Ultimate USA 1.3.0.", "Lista las salas del lobby de la comunidad (las mismas que usa Azahar). Todos necesitan Monster Hunter 4 Ultimate USA 1.3.0."},
        {"The address of a room, like 203.0.113.7:24872.", "La dirección de una sala, como 203.0.113.7:24872."},
        {"The password of the room you join, if it has one. It is not saved.", "La contraseña de la sala a la que entras, si tiene. No se guarda."},
        {"Joins the room at that address. Then, in the game, go to the Gathering Hall and choose Local Play.", "Entra a la sala de esa dirección. Después, en el juego, ve al Gathering Hall y elige Local Play."},
        {"The UDP port your room listens on. Open it on your router so others can join.", "El puerto UDP de tu sala. Ábrelo en tu router para que otros entren."},
        {"An optional password for the room you host. It is not saved.", "Una contraseña opcional para la sala que alojas. No se guarda."},
        {"Opens a room on your PC and joins it. Others join your public IP address and this port.", "Abre una sala en tu PC y entra a ella. Los demás entran a tu IP pública y este puerto."},
        {"Leaves the room you are in.", "Sales de la sala en la que estás."},
        {"Closes the menu and goes back to the game.", "Cierra el menú y vuelve al juego."}, {"Closes the game.", "Cierra el juego."},
        {"Gets the list of rooms again.", "Vuelve a pedir la lista de salas."}, {"The password of this room.", "La contraseña de esta sala."},
        {"Joins the room with that password.", "Entra a la sala con esa contraseña."},
        {"Host a room", "Alojar una sala"}, {"Join with the copied address", "Unirse con la dirección copiada"},
        {"Copy the address", "Copiar la dirección"}, {"Copy", "Copiar"}, {"local network", "red local"},
        {"Hosting. Send one of these addresses to your friends:", "Alojando. Envía una de estas direcciones a tus amigos:"},
        {"No VPN or local network found: friends need your public IP and the port open on your router.", "No encontré VPN ni red local: tus amigos necesitan tu IP pública y el puerto abierto en tu router."},
        {"Hosting. No VPN or local network found: friends need your public IP and the port open on your router.", "Alojando. No encontré VPN ni red local: tus amigos necesitan tu IP pública y el puerto abierto en tu router."},
        {"Hosting. Address copied: ", "Alojando. Dirección copiada: "}, {"Send it to your friends.", "Envíala a tus amigos."},
        {"Copy the address your friend sent (like 26.1.2.3:24872) and try again.", "Copia la dirección que te mandó tu amigo (como 26.1.2.3:24872) y vuelve a intentarlo."},
        {"* marks the rooms set up for MH4U, and they come first. You need MH4U USA 1.3.0.", "El * marca las salas preparadas para MH4U, y van primero. Necesitas MH4U USA 1.3.0."},
        {"Opens a room on your PC and copies the address to send to your friends: a Radmin VPN, Hamachi, ZeroTier or Tailscale address if you have one (no router to open), else your local network's.", "Abre una sala en tu PC y copia la dirección que debes mandar a tus amigos: la de Radmin VPN, Hamachi, ZeroTier o Tailscale si tienes (sin abrir el router), o si no la de tu red local."},
        {"Joins the room whose address you copied (Ctrl+C on your friend's message).", "Entra a la sala cuya dirección copiaste (Ctrl+C en el mensaje de tu amigo)."},
        {"Copies the address to send to your friends again.", "Vuelve a copiar la dirección para mandar a tus amigos."},
        {"Joins this room. Rooms with a password ask for it first.", "Entra a esta sala. Las salas con contraseña la piden antes."},
    };
    const auto it = kEs.find(en);
    return it == kEs.end() ? en : it->second;
}

}  // namespace ingame
