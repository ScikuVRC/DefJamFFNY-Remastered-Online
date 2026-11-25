#pragma once
#include "netplay.h"

// Live fighter select and the venue vote, in the Online Match window while the lobby is
// picking or voting (netplay.h): every player's cursor on one roster, lock-in, the clock, the
// player cards, then four venues to vote on. The host's game runs the clocks and moves on by
// itself. `width` and `height` are the space the window may take.
void ui_online_select(const NetplayLobby &lobby, float width, float height, float scale);

// Each player's colour on these screens: amber, teal, pink, blue.
unsigned ui_player_colour(int player, int alpha = 255);
