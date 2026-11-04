#pragma once
#include "imgui.h"
namespace pcui
{
inline ImVec4 gold()
{
    return ImVec4(214 / 255.f, 173 / 255.f, 82 / 255.f, 1);
}
// Fight Night accent: the launcher's PLAY button and headline kicker.
inline ImVec4 fight_red()
{
    return ImVec4(232 / 255.f, 64 / 255.f, 42 / 255.f, 1);
}
void theme(float scale);
bool slider_int(const char *label, int *value, int minimum, int maximum);
void brickwork(ImVec2 start, ImVec2 size, float scale);
void poster(ImVec2 start, ImVec2 size, float scale);
void heading(const char *text, float size);
// The face headings use (Impact), or the current font when it is not installed: for drawing with
// ImDrawList::AddText at a size in pixels.
ImFont *display();
void load_display_font();
bool play_button(const char *text, ImVec2 size, float font_size = 30.0f);
// Display-font button: filled with `fill`, or outlined in the text colour when `outline` is set.
bool accent_button(const char *text, ImVec2 size, float font_size, ImVec4 fill, bool outline = false);
// Dark diagonal stripes behind the launcher's poster panel.
void stripes(ImVec2 start, ImVec2 size, float scale);
} // namespace pcui
