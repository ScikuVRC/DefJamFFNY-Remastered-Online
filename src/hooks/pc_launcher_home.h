#pragma once
struct UiHomeResult
{
    bool play = false;
    int route = 0;          // 1 Advanced, 2 Controls, 3 Mods, 4 Setup, 5 Online Match
    int start = 0;          // with play: where the game starts (launcher_start.h)
    int user = -1;          // and with which profile
    bool quit = false;
};
UiHomeResult ui_launcher_home(float width, float height, const char *focus = nullptr);
// The full-window Home: the art behind everything and a row of tiles, drawn over the whole of
// the current window when it is big enough for it. `focus` names the control to give the pad's
// focus to, coming back from a page.
bool ui_launcher_cinematic_fits(float width, float height);
UiHomeResult ui_launcher_cinematic(const char *focus = nullptr);
void ui_home_fields();
void ui_home_devices();
