// Launcher art read from the player's own copy of the game (pc_launcher_game_art.cpp).
#pragma once
#include "pc_launcher_art.h"

// The fighter's card (the game's portrait: the render and the name logo, 281x377) from
// game/screens/screens.viv, or nullptr when the game's files do not have it.
const LauncherImage *launcher_game_fighter(int id);
