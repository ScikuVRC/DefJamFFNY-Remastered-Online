#include "pc_launcher_home.h"
#include "pc_launcher_art.h"
#include "pc_launcher_profiles.h"
#include "pc_ui_theme.h"
#include "pc_ui_actions.h"
#include "pc_mods.h"
#include "pc_ui_internal.h"
#include "input_host.h"
#include "launcher_start.h"
#include "pc_settings.h"
#include "recomp_settings.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#ifndef DEFJAM_VERSION
#define DEFJAM_VERSION "dev"
#endif

// One row (or grid) of utility buttons; `first`..`last` index into the labels below.
static void ui_home_utilities(UiHomeResult &r, const char *focus, int first, int last, int columns,
                              const char *table, float button_height = 44)
{
    static const char *labels[] = {"Advanced", "Mods", "Open Logs", "Open Folder", "Update / Repair"};
    // Advanced reads as "All settings" on the Fight Night Home; its target name stays "Advanced".
    static const char *captions[] = {"All settings", "Mods", "Open Logs", "Open Folder", "Update / Repair"};
    float s = ImGui::GetStyle().FontScaleMain;
    if (!ImGui::BeginTable(table, columns, ImGuiTableFlags_SizingStretchSame))
        return;
    for (int i = first; i <= last; i++)
    {
        ImGui::TableNextColumn();
        char id[48];
        snprintf(id, sizeof id, "%s##%s", captions[i], labels[i]);
        bool hit = ImGui::Button(id, ImVec2(-1, button_height * s));
        ui_target(labels[i]);
        if (focus && !strcmp(focus, labels[i]))
            ImGui::SetKeyboardFocusHere(-1);
        if (i == 0 && hit)
            r.route = 1;
        if (i == 1 && hit)
            r.route = 3;
        if (i == 2 && hit)
            pcui::open_path(pcui::folder_path(pcui::Folder::Logs));
        if (i == 3 && hit)
            ImGui::OpenPopup("folders");
        if (i == 3 && ImGui::BeginPopup("folders"))
        {
            const char *names[] = {"Data", "Saves", "Mods", "Installation", "Settings file"};
            pcui::Folder kinds[] = {pcui::Folder::Data, pcui::Folder::Saves, pcui::Folder::Mods,
                                    pcui::Folder::Install, pcui::Folder::Settings};
            for (int j = 0; j < 5; j++)
                if (ImGui::MenuItem(names[j]))
                    pcui::open_path(pcui::folder_path(kinds[j]), j == 4);
            ImGui::EndPopup();
        }
        if (i == 4 && hit)
            r.route = 4;
    }
    ImGui::EndTable();
}

static void ui_home_controls_row(UiHomeResult &r, const char *focus)
{
    if (ImGui::Button("Configure controls"))
        r.route = 2;
    ui_target("Configure");
    if (focus && !strcmp(focus, "Configure"))
        ImGui::SetKeyboardFocusHere(-1);
}

// PLAY plus the reserved Online button, at the cursor.
static void ui_home_play_row(UiHomeResult &r, float w, ImVec4 accent)
{
    float s = ImGui::GetStyle().FontScaleMain, buttons = 72 * s;
    ImGui::BeginDisabled(pcui::mod_busy() || pcui::mod_recovery_needed());
    r.play = pcui::accent_button("PLAY", ImVec2(std::min(240 * s, w * .4f), buttons), 40, accent);
    ui_target("Play");
    ImGui::EndDisabled();
    if (ImGui::IsWindowAppearing())
        ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, 16 * s);
    // Host or join a netplay session (pc_ui.cpp, ui_online_match).
    ImGui::BeginDisabled(pcui::mod_busy() || pcui::mod_recovery_needed());
    if (pcui::accent_button("ONLINE MATCH", ImVec2(std::min(220 * s, w * .35f), buttons), 22, accent, true))
        r.route = 5;
    ui_target("Online");
    ImGui::EndDisabled();
}

// "City in Hand" poster, drawn when the player supplied cover art from their own copy
// (<data folder>/launcher/cover_city and cover_logo, see pc_launcher_art.h).
static bool ui_home_cover_poster(UiHomeResult &r, float w, float h)
{
    const LauncherImage *city = launcher_image("cover_city"), *logo = launcher_image("cover_logo");
    if (!city || !logo)
        return false;
    float s = ImGui::GetStyle().FontScaleMain, pad = 48 * s;
    const ImVec4 amber(217 / 255.f, 119 / 255.f, 43 / 255.f, 1);
    const ImU32 ground = IM_COL32(11, 8, 6, 255), clear = IM_COL32(11, 8, 6, 0);
    ImGui::BeginChild("hero", ImVec2(w, h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    auto *d = ImGui::GetWindowDrawList();
    d->AddRectFilled(a, b, ground);

    // The city fills the right of the panel, cropped to cover its box, a little above centre.
    ImVec2 box0(a.x + w * .38f, a.y), uv0(0, 0), uv1(1, 1);
    float box_aspect = (b.x - box0.x) / h, image_aspect = city->width / city->height;
    if (image_aspect > box_aspect)
    {
        float span = box_aspect / image_aspect;
        uv0.x = (1 - span) * .5f;
        uv1.x = uv0.x + span;
    }
    else
    {
        float span = image_aspect / box_aspect;
        uv0.y = (1 - span) * .4f;
        uv1.y = uv0.y + span;
    }
    d->AddImage(city->texture, box0, b, uv0, uv1);
    float fade = (b.x - box0.x) * .6f;
    d->AddRectFilledMultiColor(box0, ImVec2(box0.x + fade, b.y), ground, clear, clear, ground);
    d->AddRectFilledMultiColor(ImVec2(a.x, b.y - 160 * s), b, clear, clear, ground, ground);

    // PLAY sits at the bottom-left (as on the Cover Art mockup); the kicker and the cover logo
    // are centred in the space above it.
    float column = std::min(440 * s, w * .55f);
    float logo_h = column * logo->height / logo->width, kicker = 20 * s, buttons = 72 * s;
    float play_y = b.y - pad - buttons;
    float y = a.y + (play_y - a.y - (kicker * 1.4f + 18 * s + logo_h)) * .5f;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    pcui::heading("THE CITY IS YOURS TO TAKE", kicker);
    ImGui::PopStyleColor();
    y += kicker * 1.4f + 18 * s;
    d->AddImage(logo->texture, ImVec2(a.x + pad, y), ImVec2(a.x + pad + column, y + logo_h));
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, play_y));
    ui_home_play_row(r, w, amber);
    ImGui::EndChild();
    return true;
}

// Fight Night poster panel: stripes, kicker, two-line headline, PLAY and Online.
static void ui_home_poster(UiHomeResult &r, float w, float h)
{
    if (ui_home_cover_poster(r, w, h))
        return;
    float s = ImGui::GetStyle().FontScaleMain, pad = 40 * s;
    ImGui::BeginChild("hero", ImVec2(w, h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImVec2 a = ImGui::GetCursorScreenPos();
    pcui::stripes(a, ImVec2(w, h), s);

    // Headline sized to the panel: two tightly set lines of display type above the button row.
    // Lines are placed explicitly; the display font's own line height spaces them too far apart.
    float buttons = 72 * s, kicker = 22 * s, leading = .9f;
    float title = std::min({132 * s, (w - 2 * pad) / 3.4f, (h - 2 * pad - buttons - kicker * 1.4f - 40 * s) / (2 * leading + .25f)});
    float block = kicker * 1.4f + title * (leading + 1.1f) + 28 * s + buttons;
    float y = a.y + h - pad - block;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    ImGui::PushStyleColor(ImGuiCol_Text, pcui::fight_red());
    pcui::heading("TONIGHT IN NEW YORK", kicker);
    ImGui::PopStyleColor();
    y += kicker * 1.4f;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    pcui::heading("FIGHT", title);
    y += title * leading;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    pcui::heading("FOR NY", title);
    y += title * 1.1f + 28 * s;

    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    ui_home_play_row(r, w, pcui::fight_red());
    ImGui::EndChild();
}

UiHomeResult ui_launcher_home(float width, float height, const char *focus)
{
    UiHomeResult r;
    float s = ImGui::GetStyle().FontScaleMain, gap = 20 * s;
    bool split = width >= 850 * s;
    if (split)
    {
        float body = std::max(height, 410 * s);
        // Wide enough for ui_home_fields' two-column grid: it needs 390 px of content width,
        // after this child's padding and a possible scrollbar.
        float right = std::clamp(width * .44f, 470 * s, 520 * s);
        ui_home_poster(r, width - right - gap, body);
        ImGui::SameLine(0, gap);
        ImGui::BeginChild("quick settings", ImVec2(0, body),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        pcui::heading("CORNER SETUP", 25 * s);
        ImGui::Spacing();
        ui_home_fields();
        ImGui::Separator();
        ui_home_devices();
        ui_home_controls_row(r, focus);
        ui_home_utilities(r, focus, 0, 1, 2, "main utilities");
        ui_home_utilities(r, focus, 2, 4, 3, "file utilities", 36);
        ImGui::EndChild();
        return r;
    }

    // Narrow windows: the launcher footer carries PLAY (pc_ui.cpp), so Home lists settings only.
    pcui::heading("FIGHT FOR NY", 32 * s);
    ImGui::Spacing();
    ImGui::BeginChild("quick settings", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    pcui::heading("CORNER SETUP", 25 * s);
    ImGui::Spacing();
    ui_home_fields();
    ImGui::Separator();
    ui_home_devices();
    ui_home_controls_row(r, focus);
    ImGui::EndChild();
    ImGui::Spacing();
    ui_home_utilities(r, focus, 0, 4, std::max(1, std::min(5, (int)(width / (145 * s)))), "utilities");
    return r;
}

// ---- The cinematic Home ------------------------------------------------------------------------
// Mockup 9 (2026-11-02): the city full-bleed behind everything, drifting slowly; the logo top left;
// what the lit tile does, large, with the profile's created fighter beside it; a row of tiles along
// the bottom and the controls under them. Left and right move along the tiles, A or Enter takes
// one, START opens the settings, B or Escape asks to quit; the mouse works throughout.
namespace
{
enum Tile
{
    CONTINUE,
    BATTLE,
    ONLINE,
    MAIN_MENU,
    PROFILES,
    MODS,
    TILES
};
// Their ui_target names. Mods keeps the old Home's, which test scripts click.
const char *const k_tile_names[TILES] = {"Continue", "Battle", "OnlineTile", "MainMenu", "Profiles", "Mods"};
const char *const DOT = " \xC2\xB7 ";
int g_lit = CONTINUE;       // the tile the big words describe: the one with the mouse, else the focus

ImU32 amber(int alpha = 255)
{
    return IM_COL32(217, 119, 43, alpha);
}
ImU32 ground(int alpha = 255)
{
    return IM_COL32(11, 8, 6, alpha);
}
const ImU32 SOFT = IM_COL32(200, 194, 186, 255);

// The city covering the window, a little larger than it so it can drift, and shade under the
// words: from the left, along the bottom, a little at the top.
void backdrop(ImDrawList *d, ImVec2 a, ImVec2 b, float s)
{
    float w = b.x - a.x, h = b.y - a.y;
    d->AddRectFilled(a, b, ground());
    if (const LauncherImage *city = launcher_image("cover_city"))
    {
        float aspect = city->width / city->height, iw = std::max(w, h * aspect) * 1.12f, ih = iw / aspect;
        float t = (float)ImGui::GetTime();
        ImVec2 p(a.x - (iw - w) * (.5f + .5f * sinf(t * .045f)), a.y - (ih - h) * (.5f + .5f * sinf(t * .031f + 1)));
        d->AddImage(city->texture, p, ImVec2(p.x + iw, p.y + ih));
    }
    else
        pcui::stripes(a, ImVec2(w, h), s);
    d->AddRectFilledMultiColor(a, ImVec2(a.x + w * .66f, b.y), ground(235), ground(0), ground(0), ground(235));
    d->AddRectFilledMultiColor(ImVec2(a.x, a.y + h * .48f), b, ground(0), ground(0), ground(248), ground(248));
    d->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + 140 * s), ground(170), ground(170), ground(0), ground(0));
}

// A line drawing for a tile, `r` square at `p`; `fill` is the tile's own colour.
void icon(ImDrawList *d, int which, ImVec2 p, float r, ImU32 col, ImU32 fill, float s)
{
    float t = 2 * s;
    ImVec2 c(p.x + r * .5f, p.y + r * .5f);
    auto at = [&](float x, float y) { return ImVec2(p.x + r * x, p.y + r * y); };
    switch (which)
    {
    case BATTLE: // two blades crossed, with their guards
        d->AddLine(at(.1f, .9f), at(.9f, .1f), col, t);
        d->AddLine(at(.9f, .9f), at(.1f, .1f), col, t);
        d->AddLine(at(.12f, .62f), at(.38f, .88f), col, t);
        d->AddLine(at(.62f, .88f), at(.88f, .62f), col, t);
        break;
    case ONLINE: // a globe
        d->AddCircle(c, r * .5f, col, 28, t);
        d->AddEllipse(c, ImVec2(r * .22f, r * .5f), col, 0, 24, t);
        d->AddLine(at(0, .5f), at(1, .5f), col, t);
        break;
    case MAIN_MENU: // a list
        for (int i = 0; i < 3; i++)
            d->AddLine(at(.05f, .2f + .3f * i), at(.95f, .2f + .3f * i), col, t);
        break;
    case PROFILES: // a head and shoulders
        d->AddCircle(at(.5f, .3f), r * .22f, col, 20, t);
        d->PathArcTo(at(.5f, 1.02f), r * .44f, 3.1416f * 1.08f, 3.1416f * 1.92f, 20);
        d->PathStroke(col, ImDrawFlags_None, t);
        break;
    case MODS: // two layers
        d->AddRect(at(.3f, 0), at(1, .7f), col, 2 * s, 0, t);
        d->AddRectFilled(at(0, .3f), at(.7f, 1), fill, 2 * s);
        d->AddRect(at(0, .3f), at(.7f, 1), col, 2 * s, 0, t);
        break;
    default: // play
        d->AddTriangleFilled(at(.2f, .1f), at(.9f, .5f), at(.2f, .9f), col);
    }
}

// Text in the display face at `size` pixels, made smaller to fit `width` when it is wider.
void display_text(ImDrawList *d, ImVec2 p, float size, ImU32 col, const char *text, float width = 0)
{
    ImFont *f = pcui::display();
    float wide = f->CalcTextSizeA(size, FLT_MAX, 0, text).x;
    if (width > 0 && wide > width)
        size *= width / wide;
    d->AddText(f, size, p, col, text);
}

std::string money(unsigned cash)
{
    std::string digits = std::to_string(cash), out = "$";
    for (size_t i = 0; i < digits.size(); i++)
    {
        if (i && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return out;
}

std::string story_text(const LauncherProfile &p)
{
    return p.story >= 100 ? "Story complete" : p.story >= 0 ? "Story " + std::to_string(p.story) + "%" : "Story";
}

// What the Select User ID screen says about a profile, and when it was last saved.
std::string profile_line(const LauncherProfile &p)
{
    std::string line = story_text(p);
    if (p.story_seconds >= 60)
    {
        char time[40];
        snprintf(time, sizeof time, "%u h %02u min", p.story_seconds / 3600, p.story_seconds / 60 % 60);
        line += DOT;
        line += time;
    }
    if (p.story >= 0)
        line += DOT + money(p.cash);
    std::string when = launcher_played_text(p.played);
    if (!when.empty())
        line += DOT + ("saved " + when);
    return line;
}

int packs_on()
{
    int n = 0;
    for (const char *section : {"textures", "mods"})
    {
        char list[RECOMP_SETTING_TEXT_MAX];
        recomp_settings_get_text(section, "packs", list, sizeof list, "");
        for (const char *p = list; *p;)
        {
            while (*p == ';' || *p == ',' || *p == ' ')
                p++;
            if (!*p)
                break;
            n++;
            while (*p && *p != ';' && *p != ',')
                p++;
        }
    }
    return n;
}

std::string status_line()
{
    int pads = 0, keyboard = 0;
    for (int i = 0; i < 4; i++)
    {
        int what = xbox_HostInputSlotInfo(i, nullptr, 0);
        pads += (what & 1) != 0;
        keyboard |= what & 2;
    }
    std::string line = pads ? std::to_string(pads) + (pads == 1 ? " controller" : " controllers")
                            : keyboard ? "Keyboard" : "No controller";
    int packs = packs_on();
    line += DOT + (packs ? std::to_string(packs) + (packs == 1 ? " pack on" : " packs on") : std::string("No packs on"));
    return line + DOT + "v" DEFJAM_VERSION;
}

struct Words
{
    const char *kicker;
    std::string title, detail, action;
};

Words words(int tile, const LauncherProfile *me, size_t profiles)
{
    switch (tile)
    {
    case CONTINUE:
        if (!me)
            return {"NEW STORY", "TAKE THE CITY",
                    "Create your fighter and fight your way up through New York's underground.",
                    "Press A to start a new story"};
        return {"CONTINUE", me->name, profile_line(*me), "Press A to continue the story"};
    case BATTLE:
        return {"BATTLE", "BATTLE",
                "One on One, Team Match, Free For All and the special matches, against the computer "
                "or a friend on this PC.",
                "Press A to choose a match"};
    case ONLINE:
        return {"ONLINE", "ONLINE MATCH",
                "Host or join a match over the internet, up to four players, with your own created fighters.",
                "Press A to host or join"};
    case MAIN_MENU:
        return {"MAIN MENU", "MAIN MENU", "The game's own menu without the intro: create a fighter, Extras, Options.",
                "Press A to start"};
    case PROFILES:
        return {"PROFILES", me ? me->name : "PROFILES",
                profiles ? std::to_string(profiles) + (profiles == 1 ? " profile" : " profiles") +
                               " on this PC. Choose whose story Continue picks up."
                         : "No profiles yet: a new story makes one.",
                "Press A to choose"};
    default:
        return {"MODS", "MODS", "HD textures, music, models and arenas: turn packs on and off and put them in order.",
                "Press A to manage mods"};
    }
}

// What a tile says about itself, under its name.
std::string tile_detail(int tile, const LauncherProfile *me, size_t profiles)
{
    switch (tile)
    {
    case CONTINUE:
        return me ? me->name + DOT + (me->story >= 100 ? "100%" : me->story >= 0 ? std::to_string(me->story) + "%" : "")
                  : "Create your fighter";
    case BATTLE:
        return "CPU or a friend";
    case ONLINE:
        return "Host or join";
    case MAIN_MENU:
        return "No intro";
    case PROFILES:
        return std::to_string(profiles) + " on this PC";
    default:
    {
        int packs = packs_on();
        return packs ? std::to_string(packs) + (packs == 1 ? " pack on" : " packs on") : "None on";
    }
    }
}

// Whose story Continue picks up.
void profile_picker(float s, float width, float height)
{
    const std::vector<LauncherProfile> &profiles = launcher_profiles();
    const LauncherProfile *me = launcher_current_profile();
    ImGui::SetNextWindowSizeConstraints(ImVec2(380 * s, 0), ImVec2(width - 60 * s, height - 60 * s));
    if (!ImGui::BeginPopupModal("Profiles##home", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    pcui::heading("WHO'S PLAYING?", 26 * s);
    ImGui::TextDisabled("Continue story plays as this profile.");
    ImGui::Spacing();
    if (profiles.empty())
        ImGui::TextWrapped("No profiles yet. Start a new story to make one.");
    float row = 58 * s, picture = row - 10 * s;
    for (const LauncherProfile &p : profiles)
    {
        ImGui::PushID(p.name.c_str());
        ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", me == &p, 0, ImVec2(440 * s, row)))
        {
            recomp_settings_set_text("launcher", "profile", p.name.c_str());
            pc_settings_save();
            ImGui::CloseCurrentPopup();
        }
        ui_target(("Profile:" + p.name).c_str());
        if (me == &p && ImGui::IsWindowAppearing())
            ImGui::SetItemDefaultFocus();
        ImDrawList *d = ImGui::GetWindowDrawList();
        ImVec2 pa(at.x + 5 * s, at.y + 5 * s);
        if (p.face)
            d->AddImageRounded(p.face->texture, pa, ImVec2(pa.x + picture, pa.y + picture), ImVec2(0, 0), ImVec2(1, 1),
                               IM_COL32_WHITE, 4 * s);
        else
            d->AddRectFilled(pa, ImVec2(pa.x + picture, pa.y + picture), IM_COL32(60, 54, 48, 255), 4 * s);
        float tx = pa.x + picture + 14 * s;
        d->AddText(pcui::display(), 22 * s, ImVec2(tx, at.y + 6 * s), me == &p ? amber() : IM_COL32_WHITE, p.name.c_str());
        d->AddText(ImVec2(tx, at.y + 32 * s), SOFT, profile_line(p).c_str());
        ImGui::PopID();
    }
    ImGui::Spacing();
    bool back = !ImGui::IsWindowAppearing() &&
                (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
    if (ImGui::Button("Close") || back)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void quit_question(UiHomeResult &r, float s)
{
    if (!ImGui::BeginPopupModal("Quit##home", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    pcui::heading("LEAVE NEW YORK?", 24 * s);
    ImGui::Spacing();
    if (ImGui::Button("Quit", ImVec2(120 * s, 0)))
    {
        r.quit = true;
        ImGui::CloseCurrentPopup();
    }
    ui_target("QuitYes");
    ImGui::SameLine();
    bool back = !ImGui::IsWindowAppearing() &&
                (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
    if (ImGui::Button("Stay", ImVec2(120 * s, 0)) || back)
        ImGui::CloseCurrentPopup();
    if (ImGui::IsWindowAppearing())
        ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
}
} // namespace

void ui_launcher_backdrop(float shade)
{
    float s = ImGui::GetStyle().FontScaleMain;
    ImVec2 a = ImGui::GetWindowPos(), size = ImGui::GetWindowSize(), b(a.x + size.x, a.y + size.y);
    ImDrawList *d = ImGui::GetWindowDrawList();
    d->PushClipRect(a, b, false);
    backdrop(d, a, b, s);
    d->AddRectFilled(a, b, ground((int)(255 * std::clamp(shade, 0.f, 1.f))));
    d->PopClipRect();
}

bool ui_launcher_cinematic_fits(float width, float height)
{
    float s = ImGui::GetStyle().FontScaleMain;
    return width >= 760 * s && height >= 470 * s;
}

UiHomeResult ui_launcher_cinematic(const char *focus)
{
    UiHomeResult r;
    float s = ImGui::GetStyle().FontScaleMain, body = ImGui::GetFontSize();
    ImVec2 a = ImGui::GetWindowPos(), size = ImGui::GetWindowSize(), b(a.x + size.x, a.y + size.y);
    float w = size.x, h = size.y, pad = std::clamp(w * .036f, 24 * s, 56 * s);
    ImDrawList *d = ImGui::GetWindowDrawList();
    const std::vector<LauncherProfile> &profiles = launcher_profiles();
    const LauncherProfile *me = launcher_current_profile();
    bool busy = pcui::mod_busy() || pcui::mod_recovery_needed();
    bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    d->PushClipRect(a, b, false);
    backdrop(d, a, b, s);

    // The top: the logo, and what a mouse wants.
    float top = 26 * s;
    if (const LauncherImage *logo = launcher_image("cover_logo"))
    {
        float lw = std::min(300 * s, w * .3f);
        d->AddImage(logo->texture, ImVec2(a.x + pad, a.y + top),
                    ImVec2(a.x + pad + lw, a.y + top + lw * logo->height / logo->width));
    }
    else
    {
        display_text(d, ImVec2(a.x + pad, a.y + top), 40 * s, IM_COL32_WHITE, "DEF JAM");
        display_text(d, ImVec2(a.x + pad, a.y + top + 42 * s), 20 * s, amber(), "FIGHT FOR NY");
    }
    {
        // Configure and Advanced are the old Home's names for these, which test scripts click.
        const char *labels[] = {"Controls", "Settings", "Quit"}, *targets[] = {"Configure", "Advanced", "Quit"};
        const ImGuiStyle &style = ImGui::GetStyle();
        float total = style.ItemSpacing.x * 2;
        for (const char *label : labels)
            total += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2;
        ImGui::SetCursorScreenPos(ImVec2(b.x - pad - total, a.y + top));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, .45f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, .14f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, .22f));
        for (int i = 0; i < 3; i++)
        {
            if (i)
                ImGui::SameLine();
            bool hit = ImGui::Button(labels[i]);
            ui_target(targets[i]);
            if (focus && !strcmp(focus, targets[i]))
                ImGui::SetKeyboardFocusHere(-1);
            if (hit && i == 2)
                r.quit = true;
            else if (hit)
                r.route = i == 0 ? 2 : 1;
        }
        ImGui::PopStyleColor(3);
    }

    // The bottom up: the controls line, the tiles (CONTINUE two wide), the words.
    float hint = body + 18 * s, tile_h = std::clamp(h * .15f, 80 * s, 116 * s), lift = 10 * s, gap = 10 * s;
    float tiles_y = b.y - hint - tile_h, unit = (w - 2 * pad - 5 * gap) / 7;
    float x0[TILES], x1[TILES], x = a.x + pad;
    int focused = -1, hovered = -1, taken = -1;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));    // the lit tile shows the focus
    for (int i = 0; i < TILES; i++)
    {
        x0[i] = x;
        x1[i] = x0[i] + (i == CONTINUE ? 2 * unit : unit);
        x = x1[i] + gap;
        ImGui::SetCursorScreenPos(ImVec2(x0[i], tiles_y - lift));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tile", ImVec2(x1[i] - x0[i], tile_h + lift)))
            taken = i;
        ImGui::PopID();
        ui_target(k_tile_names[i]);
        if (i == CONTINUE && ImGui::IsWindowAppearing())
            ImGui::SetItemDefaultFocus();
        if (focus && !strcmp(focus, k_tile_names[i]))
            ImGui::SetKeyboardFocusHere(-1);
        if (ImGui::IsItemFocused())
            focused = i;
        if (ImGui::IsItemHovered())
            hovered = i;
    }
    ImGui::PopStyleColor();
    if (hovered >= 0)
        g_lit = hovered;
    else if (focused >= 0)
        g_lit = focused;

    // The profile's created fighter beside the words, when they are about it.
    float words_bottom = tiles_y - lift - 22 * s;
    if ((g_lit == CONTINUE || g_lit == PROFILES) && me && me->fighter)
    {
        // At most twice its 128 pixels: the game drew it without smoothing, and larger it turns blocky.
        float ph = std::min(words_bottom - (a.y + 90 * s), 256 * s), cx = b.x - pad - std::max(ph * .5f, w * .17f);
        ImVec2 fa(cx - ph * .5f, words_bottom + 14 * s - ph), fb(cx + ph * .5f, words_bottom + 14 * s);
        for (int k = 6; k > 0; k--)
            d->AddCircleFilled(ImVec2(cx, fa.y + ph * .5f), ph * (.22f + .06f * k), amber(9), 48);
        d->AddEllipseFilled(ImVec2(cx, fb.y - ph * .02f), ImVec2(ph * .28f, ph * .045f), ground(170));
        d->AddImage(me->fighter->texture, fa, fb);
    }

    for (int i = 0; i < TILES; i++)
    {
        bool lit = i == g_lit, off = busy && i <= MAIN_MENU;
        ImVec2 ta(x0[i], tiles_y - (lit ? lift : 0)), tb(x1[i], tiles_y + tile_h);
        ImU32 fill = lit ? IM_COL32(38, 31, 26, 238) : ground(212), ink = off ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;
        float inner = 12 * s, label = 21 * s;
        d->AddRectFilled(ta, tb, fill, 6 * s);
        d->AddRect(ta, tb, lit ? amber() : IM_COL32(255, 255, 255, 30), 6 * s, 0, lit ? 2 * s : 1 * s);
        d->PushClipRect(ta, tb, true);
        std::string detail = tile_detail(i, me, profiles.size());
        float tx = ta.x + inner;
        if (i == CONTINUE && me && me->face)
        {
            float ph = std::min(tb.y - ta.y - 2 * inner, 64 * s), py = (ta.y + tb.y - ph) * .5f;
            d->AddImageRounded(me->face->texture, ImVec2(tx, py), ImVec2(tx + ph, py + ph), ImVec2(0, 0), ImVec2(1, 1),
                               off ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE, 4 * s);
            tx += ph + inner;
        }
        else
            icon(d, i, ImVec2(tx, ta.y + inner), 24 * s, lit && !off ? amber() : ink, fill, s);
        const char *name = i == CONTINUE ? (me ? "CONTINUE STORY" : "NEW STORY")
                           : i == BATTLE ? "BATTLE" : i == ONLINE ? "ONLINE" : i == MAIN_MENU ? "MAIN MENU"
                           : i == PROFILES ? "PROFILES" : "MODS";
        float ly = tb.y - inner - body - 4 * s - label;
        display_text(d, ImVec2(tx, ly), label, ink, name, tb.x - tx - inner);
        d->AddText(ImVec2(tx, ly + label + 4 * s), off ? IM_COL32(200, 194, 186, 90) : SOFT, detail.c_str());
        d->PopClipRect();
    }

    // The words: what the lit tile does.
    {
        Words say = words(g_lit, me, profiles.size());
        ImFont *f = ImGui::GetFont();
        float ww = std::min(w * .52f, 640 * s), wx = a.x + pad, kick = 20 * s;
        float title = std::clamp(h * .12f, 46 * s, 100 * s);
        float detail_h = f->CalcTextSizeA(body, FLT_MAX, ww, say.detail.c_str()).y;
        float y = words_bottom - (kick + 6 * s + title + 10 * s + detail_h + 14 * s + body);
        display_text(d, ImVec2(wx, y), kick, amber(), say.kicker);
        y += kick + 6 * s;
        display_text(d, ImVec2(wx, y), title, IM_COL32_WHITE, say.title.c_str(), ww);
        y += title + 10 * s;
        d->AddText(f, body, ImVec2(wx, y), IM_COL32(226, 220, 210, 255), say.detail.c_str(), nullptr, ww);
        y += detail_h + 14 * s;
        bool off = busy && g_lit <= MAIN_MENU;
        d->AddText(f, body, ImVec2(wx, y), amber(), off ? "Wait for the mods to finish changing" : say.action.c_str());
    }

    // The controls line.
    {
        float hy = b.y - hint + 7 * s, hx = a.x + pad;
        const char *keys[][2] = {{"LEFT / RIGHT", "move"}, {"A", "select"}, {"START", "settings"}, {"B", "quit"}};
        for (const auto &k : keys)
        {
            d->AddText(ImVec2(hx, hy), amber(), k[0]);
            hx += ImGui::CalcTextSize(k[0]).x + 6 * s;
            d->AddText(ImVec2(hx, hy), SOFT, k[1]);
            hx += ImGui::CalcTextSize(k[1]).x + 22 * s;
        }
        std::string status = busy ? pcui::mod_status() : status_line();
        float sw = ImGui::CalcTextSize(status.c_str()).x;
        d->PushClipRect(ImVec2(hx, hy), ImVec2(b.x - pad, hy + body + 4 * s), true);
        d->AddText(ImVec2(std::max(hx, b.x - pad - sw), hy), busy ? amber() : IM_COL32(160, 154, 146, 255), status.c_str());
        d->PopClipRect();
    }
    d->PopClipRect();

    if (taken >= 0 && !(busy && taken <= MAIN_MENU))
    {
        switch (taken)
        {
        case CONTINUE:
            r.play = true;
            r.start = LAUNCHER_START_STORY;
            r.user = me ? me->user : -1;
            break;
        case BATTLE:
            // Its players pick their user IDs after the match type, on the game's own screen.
            r.play = true;
            r.start = LAUNCHER_START_BATTLE;
            break;
        case ONLINE:
            r.route = 5;
            break;
        case MAIN_MENU:
            r.play = true;
            r.start = LAUNCHER_START_MENU;
            break;
        case PROFILES:
            ImGui::OpenPopup("Profiles##home");
            break;
        default:
            r.route = 3;
        }
    }
    if (!popup && !ui_capturing())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
            r.route = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            ImGui::OpenPopup("Quit##home");
    }
    profile_picker(s, w, h);
    quit_question(r, s);
    return r;
}
