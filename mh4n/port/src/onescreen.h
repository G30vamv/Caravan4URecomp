// One screen: the game's touch-screen menus drawn on the top screen by the game itself (single_screen in mh4u.ini).
#pragma once

#include <cstdint>
#include <string>

struct RCtx;

namespace Port::OneScreen {

/// hide_help: the game's help message box (ui006_help00 and ui006_help01: the black box with a line of text a menu shows for
/// the selected row) is not drawn on either screen (it overlapped menus, and the title screen's touch screen showed
/// nothing else).
void SetHideHelp(bool hide);

/// True while the village's touch screen shows its map icon on the plain screen (not in a quest, no touch menu in force): the
/// shell's map button taps the icon then (map_key / map_pad).
bool MapTapAvailable();
/// True while the world map is open (its Back button on the touch screen can be tapped to close it).
bool MapOpen();

/// hooks.cpp's sMhMain::frame, before the game's frame. Returns a line for the log, or "".
std::string Frame(RCtx* ctx, bool on);

/// hooks.cpp's nLayout::objectConstants, after the game wrote the object's constants at `constants` (x, y, z, w, then
/// the colour) for the layout pass of `screen` (0 the top screen, 7 the touch screen).
/// `hud` (scale, tx, ty; or null): a move for an object no placed group has one for (the hunting HUD's smaller size:
/// pixels p of the top screen go to scale * p + (tx, ty) about the screen's centre, tx right and ty down in pixels).
void ObjectConstants(RCtx* ctx, std::uint32_t object, std::uint32_t draw, std::uint32_t constants,
                     std::uint32_t screen, const float* hud = nullptr);

}  // namespace Port::OneScreen
