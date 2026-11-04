#include "pc_ui_theme.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstdlib>
#include <string>

namespace pcui
{
static ImFont *display_font = nullptr;
void load_display_font()
{
    display_font = nullptr;
#if defined(_WIN32)
    const char *root = getenv("WINDIR");
    std::string candidate = std::string(root ? root : "C:/Windows") + "/Fonts/impact.ttf";
    const char *fonts[] = {candidate.c_str(), nullptr};
#elif defined(__APPLE__)
    const char *fonts[] = {"/System/Library/Fonts/Supplemental/Impact.ttf",
                           "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf", nullptr};
#else
    const char *fonts[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
                           "/usr/share/fonts/TTF/DejaVuSansCondensed-Bold.ttf", nullptr};
#endif
    for (int i = 0; fonts[i]; i++)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(fonts[i], ec))
        {
            display_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(fonts[i], 22);
            if (display_font)
                break;
        }
    }
}
void theme(float scale)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark();
    s.WindowPadding = ImVec2(22, 18);
    s.FramePadding = ImVec2(12, 9);
    s.ItemSpacing = ImVec2(12, 10);
    s.WindowRounding = 0;
    s.ChildRounding = 8;
    s.PopupRounding = 8;
    s.FrameRounding = 5;
    s.GrabRounding = 4;
    s.TabRounding = 5;
    s.ScrollbarRounding = 6;
    // A hairline round every field, so a text box or a check box shows on any panel.
    s.FrameBorderSize = 1;
    s.TabBarOverlineSize = 2;
    // The Home's colours (pc_launcher_home.cpp): a warm near-black, translucent panels over the
    // city, white lines and fills at low strength, amber for what is chosen or active.
    auto white = [](float a) { return ImVec4(1, 1, 1, a); };
    auto lit = [](float a) { ImVec4 c = amber(); c.w = a; return c; };
    s.Colors[ImGuiCol_Text] = ImVec4(.914f, .886f, .839f, 1);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(.647f, .616f, .576f, 1);
    s.Colors[ImGuiCol_WindowBg] = ImVec4(.055f, .043f, .035f, 1);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(.094f, .078f, .067f, .88f);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(.102f, .086f, .074f, .98f);
    s.Colors[ImGuiCol_TitleBg] = ImVec4(.16f, .14f, .12f, 1);
    s.Colors[ImGuiCol_TitleBgActive] = ImVec4(.24f, .17f, .11f, 1);
    s.Colors[ImGuiCol_TitleBgCollapsed] = s.Colors[ImGuiCol_TitleBg];
    s.Colors[ImGuiCol_Border] = white(.11f);
    s.Colors[ImGuiCol_Separator] = white(.12f);
    s.Colors[ImGuiCol_SeparatorHovered] = lit(.6f);
    s.Colors[ImGuiCol_SeparatorActive] = amber();
    s.Colors[ImGuiCol_FrameBg] = white(.06f);
    s.Colors[ImGuiCol_FrameBgHovered] = white(.11f);
    s.Colors[ImGuiCol_FrameBgActive] = lit(.24f);
    s.Colors[ImGuiCol_Button] = white(.08f);
    s.Colors[ImGuiCol_ButtonHovered] = white(.14f);
    s.Colors[ImGuiCol_ButtonActive] = lit(.36f);
    s.Colors[ImGuiCol_Header] = lit(.20f);
    s.Colors[ImGuiCol_HeaderHovered] = lit(.30f);
    s.Colors[ImGuiCol_HeaderActive] = lit(.40f);
    s.Colors[ImGuiCol_Tab] = white(.04f);
    s.Colors[ImGuiCol_TabHovered] = white(.13f);
    s.Colors[ImGuiCol_TabSelected] = lit(.20f);
    s.Colors[ImGuiCol_CheckboxSelectedBg] = lit(.24f);
    s.Colors[ImGuiCol_TabDimmed] = s.Colors[ImGuiCol_Tab];
    s.Colors[ImGuiCol_TabDimmedSelected] = s.Colors[ImGuiCol_TabSelected];
    s.Colors[ImGuiCol_TabSelectedOverline] = amber();
    s.Colors[ImGuiCol_TabDimmedSelectedOverline] = amber();
    s.Colors[ImGuiCol_TableHeaderBg] = white(.05f);
    s.Colors[ImGuiCol_TableBorderStrong] = white(.12f);
    s.Colors[ImGuiCol_TableBorderLight] = white(.07f);
    s.Colors[ImGuiCol_TableRowBgAlt] = white(.025f);
    s.Colors[ImGuiCol_ScrollbarBg] = white(0);
    s.Colors[ImGuiCol_ScrollbarGrab] = white(.14f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = white(.22f);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = lit(.7f);
    s.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, .6f);
    s.Colors[ImGuiCol_TextSelectedBg] = lit(.35f);
    s.Colors[ImGuiCol_CheckMark] = amber();
    s.Colors[ImGuiCol_SliderGrab] = amber();
    s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(.95f, .6f, .31f, 1);
    s.Colors[ImGuiCol_NavCursor] = amber();
    s.ScaleAllSizes(scale);
    s.FontScaleMain = scale;
}
bool slider_int(const char *label, int *value, int minimum, int maximum)
{
    // Keep the full slider track visible against the page without brightening other fields.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(.24f, .22f, .19f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(.31f, .27f, .21f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(.37f, .31f, .23f, 1));
    bool changed = ImGui::SliderInt(label, value, minimum, maximum);
    ImGui::PopStyleColor(3);
    return changed;
}
ImFont *display()
{
    return display_font ? display_font : ImGui::GetFont();
}
void heading(const char *text, float size)
{
    ImGui::PushFont(display_font, size / ImGui::GetStyle().FontScaleMain);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}
void section(const char *text)
{
    char upper[128];
    size_t n = 0;
    for (; text[n] && n < sizeof upper - 1; n++)
        upper[n] = (char)toupper((unsigned char)text[n]);
    upper[n] = 0;
    float s = ImGui::GetStyle().FontScaleMain;
    // Room above it, except at the top of a page or panel.
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + ImGui::GetScrollY() + 1)
        ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::PushStyleColor(ImGuiCol_Text, amber());
    heading(upper, 17 * s);
    ImGui::PopStyleColor();
    ImGui::Separator();
}
void brickwork(ImVec2 a, ImVec2 size, float s)
{
    auto *d = ImGui::GetWindowDrawList();
    d->PushClipRect(a, ImVec2(a.x + size.x, a.y + size.y), true);
    for (int row = 0; row < size.y / (46 * s) + 1; row++)
        for (int col = -1; col < size.x / (118 * s) + 1; col++)
        {
            float x = a.x + col * 118 * s + (row % 2 ? 59 * s : 0), y = a.y + row * 46 * s;
            d->AddRect(ImVec2(x, y), ImVec2(x + 118 * s, y + 46 * s), IM_COL32(115, 103, 80, 22));
        }
    d->PopClipRect();
}
void poster(ImVec2 a, ImVec2 size, float s)
{
    auto *d = ImGui::GetWindowDrawList();
    ImU32 paper = IM_COL32(233, 223, 200, 255), ink = IM_COL32(22, 21, 19, 255);
    float bottom = a.y + size.y;
    d->AddRectFilled(a, ImVec2(a.x + size.x, bottom - 10 * s), paper);
    for (int i = 0; i < 9; i++)
    {
        float x = a.x + size.x * i / 9.f, w = size.x / 9.f;
        d->AddTriangleFilled(ImVec2(x, bottom - 10 * s), ImVec2(x + w, bottom - 10 * s),
                             ImVec2(x + w * .42f, bottom - (i % 3) * 3 * s), paper);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + 18 * s, a.y + 16 * s));
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    heading("DEF JAM", 25 * s);
    d->AddLine(ImVec2(a.x + 18 * s, a.y + 51 * s), ImVec2(a.x + 110 * s, a.y + 51 * s), ink, 3 * s);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 18 * s, a.y + 66 * s));
    float title = std::min({70 * s, (size.x - 36 * s) / 3.25f, (size.y - 122 * s) / 2.35f});
    heading("FIGHT", title);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 18 * s, ImGui::GetCursorScreenPos().y));
    heading("FOR NY", title);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 18 * s, bottom - 42 * s));
    ImGui::PushFont(nullptr, 11);
    ImGui::TextUnformatted("THE STREETS ARE WAITING.");
    ImGui::PopFont();
    ImGui::PopStyleColor();
}
void stripes(ImVec2 a, ImVec2 size, float s)
{
    auto *d = ImGui::GetWindowDrawList();
    ImVec2 b(a.x + size.x, a.y + size.y);
    d->AddRectFilled(a, b, IM_COL32(27, 27, 27, 255));
    d->PushClipRect(a, b, true);
    // 45-degree bands, 22 px wide at scale 1, alternating two near-blacks.
    float band = 22 * s;
    for (float x = a.x - size.y; x < b.x; x += band * 2)
        d->AddQuadFilled(ImVec2(x, b.y), ImVec2(x + band, b.y), ImVec2(x + band + size.y, a.y),
                         ImVec2(x + size.y, a.y), IM_COL32(31, 31, 31, 255));
    d->PopClipRect();
}
bool accent_button(const char *text, ImVec2 size, float font_size, ImVec4 fill, bool outline)
{
    ImVec4 text_color = ImGui::GetStyle().Colors[ImGuiCol_Text];
    if (outline)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, .08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, .14f));
        ImGui::PushStyleColor(ImGuiCol_Text, text_color);
        ImGui::PushStyleColor(ImGuiCol_Border, text_color);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2 * ImGui::GetStyle().FontScaleMain);
    }
    else
    {
        auto shade = [&](float f) {
            return ImVec4(std::min(1.f, fill.x * f), std::min(1.f, fill.y * f), std::min(1.f, fill.z * f), 1);
        };
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, shade(1.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, shade(.85f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.071f, .071f, .071f, 1));
        ImGui::PushStyleColor(ImGuiCol_Border, fill);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    ImGui::PushFont(display_font, font_size);
    bool hit = ImGui::Button(text, size);
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    return hit;
}
bool play_button(const char *text, ImVec2 size, float font_size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, amber());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.95f, .6f, .31f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.72f, .38f, .12f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.071f, .055f, .043f, 1));
    ImGui::PushStyleColor(ImGuiCol_Border, amber());
    ImGui::PushFont(display_font, font_size);
    bool hit = ImGui::Button(text, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(5);
    return hit;
}
} // namespace pcui
