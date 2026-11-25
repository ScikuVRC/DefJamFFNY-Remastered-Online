// Live fighter select and the venue vote (pc_launcher_select.h). The roster is a grid of
// tiles; each player's cursor is a border in their colour, everyone sees everyone's move, and a
// fighter someone has locked in is theirs. Portraits come from the player's own copy of the game
// (pc_launcher_game_art.cpp); venue pictures are saved during their first fight in each venue
// (pc_venue_art.c). launcher/fighters/<id> and launcher/venues/<id> in the data folder take
// precedence; without a picture a tile shows the fighter's initials.
#include "pc_launcher_select.h"
#include "pc_launcher_art.h"
#include "pc_launcher_game_art.h"
#include "pc_launcher_online.h"
#include "pc_launcher_profiles.h"
#include "pc_ui_internal.h"
#include "pc_ui_theme.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>

namespace
{
const ImU32 GROUND = IM_COL32(11, 8, 6, 255), TILE = IM_COL32(38, 31, 26, 255), SHADE = IM_COL32(60, 54, 48, 255),
            HAIR = IM_COL32(255, 255, 255, 30), INK = IM_COL32(226, 220, 210, 255),
            SOFT = IM_COL32(160, 154, 146, 255);

struct Style
{
    const char *tab, *suffix;   // the tab's name and how fighter.xml's names end for it
};
const Style STYLES[] = {{"All", nullptr},          {"Street", "(Street)"},       {"Martial Arts", "(Martial Arts)"},
                        {"Kickboxing", "(Kickboxing)"}, {"Wrestling", "(Wrestling)"}, {"Submissions", "(Submissions)"}};

// "Blaze (Street)" -> "Blaze"
std::string bare_name(const char *name)
{
    std::string s = name ? name : "?";
    size_t paren = s.find(" (");
    return paren == std::string::npos ? s : s.substr(0, paren);
}

std::string initials(const std::string &name)
{
    std::string out;
    bool start = true;
    for (char c : name)
    {
        if (start && c != ' ' && out.size() < 2)
            out += (char)toupper((unsigned char)c);
        start = c == ' ' || c == '-';
    }
    return out.empty() ? "?" : out;
}

const LauncherImage *art(const char *kind, int id)
{
    char name[48];
    snprintf(name, sizeof name, "%s/%d", kind, id);
    if (const LauncherImage *img = launcher_image(name))
        return img;
    // No picture of the player's own: the fighter's portrait from their copy of the game.
    return strcmp(kind, "fighters") ? nullptr : launcher_game_fighter(id);
}

// The part of a fighter's card that is the fighter: the card is the game's portrait, the render
// with the name logo at its top right.
const ImVec4 BUST(0.0f, 0.0f, 0.62f, 0.62f), WHOLE(0, 0, 1, 1);

// An image filling `a`-`b`, cropped to keep its shape: `part` is the region of the image to use
// (0-1), `anchor` where the crop sits in it (0 top or left, 0.5 the middle).
void cover(ImDrawList *d, const LauncherImage *img, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE,
           ImVec4 part = WHOLE, float anchor_y = 0.5f, float rounding = 0)
{
    float pw = img->width * (part.z - part.x), ph = img->height * (part.w - part.y);
    float w = b.x - a.x, h = b.y - a.y, s = std::max(w / pw, h / ph);
    float cu = (pw * s - w) / (pw * s) * (part.z - part.x), cv = (ph * s - h) / (ph * s) * (part.w - part.y);
    ImVec2 uv0(part.x + cu / 2, part.y + cv * anchor_y);
    ImVec2 uv1(uv0.x + (part.z - part.x) - cu, uv0.y + (part.w - part.y) - cv);
    d->AddImageRounded(img->texture, a, b, uv0, uv1, tint, rounding);
}

// Darkening towards the bottom of `a`-`b`, for text over a picture.
void fade(ImDrawList *d, ImVec2 a, ImVec2 b, int alpha = 230)
{
    d->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, alpha),
                               IM_COL32(0, 0, 0, alpha));
}

// Text in the display font, with a shadow.
void headline(ImDrawList *d, float size, ImVec2 at, ImU32 col, const char *text)
{
    ImFont *f = pcui::display();
    d->AddText(f, size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200), text);
    d->AddText(f, size, at, col, text);
}

// A figure where a portrait would be.
void silhouette(ImDrawList *d, ImVec2 a, ImVec2 b, const std::string &name, float scale)
{
    float w = b.x - a.x, h = b.y - a.y, cx = a.x + w / 2;
    d->AddCircleFilled(ImVec2(cx, a.y + h * 0.33f), w * 0.17f, SHADE);
    d->AddRectFilled(ImVec2(cx - w * 0.3f, a.y + h * 0.55f), ImVec2(cx + w * 0.3f, b.y), SHADE, 10 * scale,
                     ImDrawFlags_RoundCornersTop);
    std::string ini = initials(name);
    ImFont *f = pcui::display();
    float size = w * 0.22f;
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, ini.c_str());
    d->AddText(f, size, ImVec2(cx - ts.x / 2, a.y + h * 0.33f - ts.y / 2), IM_COL32(200, 194, 186, 200), ini.c_str());
}

// Text centred in a width, shortened with "..." to fit.
void centred(ImDrawList *d, ImVec2 at, float width, ImU32 col, const std::string &text)
{
    std::string s = text;
    while (s.size() > 3 && ImGui::CalcTextSize(s.c_str()).x > width)
        s = s.substr(0, s.size() - 4) + "...";
    float x = at.x + (width - ImGui::CalcTextSize(s.c_str()).x) / 2;
    d->AddText(ImVec2(x, at.y), col, s.c_str());
}

const char *player_label(const NetplayLobby &l, int p)
{
    static char buf[NETPLAY_MAX_PLAYERS][32];
    snprintf(buf[p], sizeof buf[p], "%s", p == l.local_player ? "You" : l.name[p]);
    return buf[p];
}

// The slot each player has (settle_match's order: players fill the slots, host first).
int slot_of(const NetplayMatch &m, int player)
{
    for (int i = 0; i < NETPLAY_MAX_PLAYERS; i++)
        if (m.owner[i] == player)
            return i;
    return -1;
}

std::string fighter_text(const NetplayMatch &m, int slot, const NetplayChoice *list, int count)
{
    if (m.fighter[slot] == NETPLAY_CREATED_FIGHTER)
        return std::string(m.profile[slot]) + " (created)";
    return bare_name(netplay_choice_name(list, count, m.fighter[slot]));
}

// The big clock in the corner.
void clock(ImDrawList *d, ImVec2 right_top, int seconds, float scale)
{
    if (seconds < 0)
        return;
    char t[8];
    snprintf(t, sizeof t, "%d", seconds);
    ImFont *f = pcui::display();
    float size = 40 * scale;
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, t);
    ImU32 col = seconds <= 5 ? IM_COL32(232, 64, 42, 255) : ImGui::GetColorU32(pcui::amber());
    d->AddText(f, size, ImVec2(right_top.x - ts.x, right_top.y), col, t);
}

void title(ImDrawList *d, ImVec2 at, const char *text, const char *kicker, float scale)
{
    d->AddRectFilled(ImVec2(at.x, at.y + 4 * scale), ImVec2(at.x + 5 * scale, at.y + 30 * scale),
                     ImGui::GetColorU32(pcui::amber()));
    headline(d, 30 * scale, ImVec2(at.x + 14 * scale, at.y), IM_COL32_WHITE, text);
    if (kicker && *kicker)
        d->AddText(ImVec2(at.x + 14 * scale, at.y + 36 * scale), SOFT, kicker);
}

// One player's card along the bottom: their colour, name, fighter, and where they are.
void player_card(ImDrawList *d, ImVec2 a, ImVec2 b, const NetplayLobby &l, int slot, int fighter, bool locked,
                 const NetplayChoice *list, int count, float scale)
{
    const NetplayMatch &m = l.match;
    int owner = m.owner[slot];
    ImU32 col = owner >= 0 ? ui_player_colour(owner) : SOFT;
    float r = 6 * scale;
    // The card: a wash of the player's colour behind their fighter, a stripe of it along the top.
    d->AddRectFilled(a, b, TILE, r);
    d->AddRectFilledMultiColor(a, ImVec2(b.x, b.y), owner >= 0 ? ui_player_colour(owner, 70) : IM_COL32(0, 0, 0, 0),
                               IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                               owner >= 0 ? ui_player_colour(owner, 25) : IM_COL32(0, 0, 0, 0));
    std::string name;
    if (fighter == NETPLAY_CREATED_FIGHTER)
        name = std::string(m.profile[slot]);
    else
        name = bare_name(netplay_choice_name(list, count, fighter));
    float pic_h = b.y - a.y, pic_w = std::min((b.x - a.x) * 0.45f, pic_h * 0.745f);
    ImVec2 pa(a.x, a.y), pb(a.x + pic_w, b.y);
    if (const LauncherImage *img = fighter > 0 && fighter != NETPLAY_CREATED_FIGHTER ? art("fighters", fighter) : nullptr)
        cover(d, img, pa, pb, locked || owner < 0 ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 190),
              pic_h > 120 * scale ? ImVec4(0, 0, 1, 0.95f) : BUST, 0.0f, r);
    else
        silhouette(d, ImVec2(pa.x + 6 * scale, pa.y + 6 * scale), ImVec2(pb.x - 6 * scale, pb.y), name, scale);
    d->AddRectFilled(a, ImVec2(b.x, a.y + 3 * scale), col, r, ImDrawFlags_RoundCornersTop);
    d->AddRect(a, b, owner >= 0 && locked ? col : HAIR, r, 0, owner >= 0 && locked ? 2 * scale : 1);
    float x = pb.x + 10 * scale, y = a.y + 10 * scale, line = ImGui::GetTextLineHeight();
    char who[64];
    if (owner >= 0)
        snprintf(who, sizeof who, "%s%s", player_label(l, owner), owner == 0 ? " (host)" : "");
    else
        snprintf(who, sizeof who, "CPU");
    d->AddText(ImVec2(x, y), col, who);
    if (m.type == 1)
    {
        char team[16];
        snprintf(team, sizeof team, "Team %d", m.team[slot] + 1);
        d->AddText(ImVec2(b.x - ImGui::CalcTextSize(team).x - 8 * scale, y), SOFT, team);
    }
    float big = std::min(26 * scale, (b.y - a.y) * 0.3f);
    std::string shown = name;
    while (shown.size() > 3 && pcui::display()->CalcTextSizeA(big, FLT_MAX, 0, shown.c_str()).x > b.x - x - 8 * scale)
        shown = shown.substr(0, shown.size() - 4) + "...";
    headline(d, big, ImVec2(x, y + line + 4 * scale), IM_COL32_WHITE, shown.c_str());
    const char *where = owner < 0 ? "COMPUTER" : locked ? "LOCKED IN" : "CHOOSING...";
    ImVec2 ws = ImGui::CalcTextSize(where);
    bool small = b.y - a.y < 100 * scale;
    ImVec2 wa = small ? ImVec2(b.x - ws.x - 14 * scale, (a.y + b.y - ws.y) / 2) : ImVec2(x, b.y - ws.y - 12 * scale);
    if (owner >= 0 && locked)
    {
        d->AddRectFilled(ImVec2(wa.x - 5 * scale, wa.y - 3 * scale), ImVec2(wa.x + ws.x + 5 * scale, wa.y + ws.y + 3 * scale),
                         col, 3 * scale);
        d->AddText(wa, GROUND, where);
    }
    else
        d->AddText(wa, SOFT, where);
    if (owner == l.local_player && b.y - a.y > 100 * scale)
    {
        std::string rec = ui_online_summary();
        if (!rec.empty())
            d->AddText(ImVec2(x, y + line + big + 10 * scale), SOFT, rec.c_str());
    }
}

void cards(const NetplayLobby &l, const NetplaySelect &sel, int slots, const NetplayChoice *list, int count,
           float width, float height, float scale)
{
    ImDrawList *d = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    float gap = 8 * scale, w = (width - gap * (slots - 1)) / slots;
    for (int s = 0; s < slots; s++)
    {
        int owner = l.match.owner[s];
        bool locked = owner >= 0 && l.ready[owner];
        int fighter = owner >= 0 && !locked && sel.cursor[owner] > 0 ? sel.cursor[owner] : l.match.fighter[s];
        ImVec2 a(at.x + s * (w + gap), at.y);
        player_card(d, a, ImVec2(a.x + w, a.y + height), l, s, fighter, locked || owner < 0, list, count, scale);
    }
    ImGui::Dummy(ImVec2(width, height));
}

// ---- Fighter select ------------------------------------------------------------------------
void ui_pick(const NetplayLobby &l, const NetplaySelect &sel, float width, float height, float scale)
{
    static int tab = 0;
    static ImGuiID last_focus = 0;
    const NetplayChoice *fighters;
    int count = netplay_fighters(&fighters);
    const NetplayMatchType *type = netplay_match_type(l.match.type);
    int slots = type ? type->fighters : 2;
    int me = l.local_player, my_slot = slot_of(l.match, me);
    bool locked = me >= 0 && l.ready[me];
    ImDrawList *d = ImGui::GetWindowDrawList();
    ImVec2 top = ImGui::GetCursorScreenPos();

    char kicker[96];
    snprintf(kicker, sizeof kicker, "%s  -  %d players", type ? type->name : "Match", l.players);
    title(d, top, "CHOOSE YOUR FIGHTER", kicker, scale);
    clock(d, ImVec2(top.x + width, top.y), sel.seconds_left, scale);
    ImGui::Dummy(ImVec2(width, 58 * scale));

    // Who has which fighter locked in: theirs, nobody else's.
    int owner_of[1000];
    std::fill(owner_of, owner_of + 1000, -1);
    for (int s = 0; s < slots; s++)
    {
        int o = l.match.owner[s], f = l.match.fighter[s];
        if (f > 0 && f < 1000 && f != NETPLAY_CREATED_FIGHTER && (o < 0 || l.ready[o]))
            owner_of[f] = o < 0 ? 99 : o;
    }

    // The style tabs.
    for (int t = 0; t < (int)(sizeof STYLES / sizeof STYLES[0]); t++)
    {
        if (t)
            ImGui::SameLine();
        bool on = tab == t;
        if (on)
            ImGui::PushStyleColor(ImGuiCol_Button, pcui::amber());
        if (ImGui::SmallButton(STYLES[t].tab))
            tab = t;
        if (on)
            ImGui::PopStyleColor();
    }

    // The roster.
    float cards_h = std::clamp(height * 0.24f, 96 * scale, 170 * scale), grid_h = height - 58 * scale - ImGui::GetFrameHeightWithSpacing() * 2 - cards_h - 16 * scale;
    ImGui::BeginChild("roster", ImVec2(width, std::max(160.0f * scale, grid_h)), ImGuiChildFlags_NavFlattened, 0);
    float tile_w = 86 * scale, tile_h = 104 * scale, gap = 6 * scale;
    int cols = std::max(1, (int)((ImGui::GetContentRegionAvail().x + gap) / (tile_w + gap)));
    tile_w = (ImGui::GetContentRegionAvail().x - gap * (cols - 1)) / cols;
    tile_h = tile_w * 1.21f;
    std::vector<int> shown;
    shown.push_back(-1);        // Random
    for (int i = 0; i < count; i++)
        if (fighters[i].id > 0 && (!STYLES[tab].suffix || strstr(fighters[i].name, STYLES[tab].suffix)))
            shown.push_back(i);
    d = ImGui::GetWindowDrawList();
    for (size_t k = 0; k < shown.size(); k++)
    {
        int i = shown[k];
        int id = i >= 0 ? fighters[i].id : -1;
        std::string name = i >= 0 ? bare_name(fighters[i].name) : "Random";
        if (k % cols)
            ImGui::SameLine(0, gap);
        ImGui::PushID((int)k);
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + tile_w, a.y + tile_h);
        bool clicked = ImGui::InvisibleButton("tile", ImVec2(tile_w, tile_h));
        bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused();
        int held = id > 0 && id < 1000 ? owner_of[id] : -1;
        bool theirs = held >= 0 && held != me;
        // The cursor follows the mouse, and the keyboard or pad.
        if (!locked && id > 0)
        {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            if ((hovered && (delta.x != 0 || delta.y != 0)) || (focused && ImGui::GetItemID() != last_focus))
                netplay_set_cursor(id);
        }
        if (focused)
            last_focus = ImGui::GetItemID();
        if (clicked && !locked && my_slot >= 0)
        {
            int pick = id;
            if (id < 0)
            {
                // Random: any fighter nobody has.
                std::vector<int> free;
                for (int j = 0; j < count; j++)
                    if (owner_of[fighters[j].id] < 0)
                        free.push_back(fighters[j].id);
                pick = free.empty() ? 0 : free[(size_t)rand() % free.size()];
            }
            if (pick > 0 && !theirs)
            {
                netplay_set_cursor(pick);
                netplay_pick(pick);
                netplay_set_ready(1);
            }
        }

        // The tile: the fighter's portrait filling it, the name over a fade at the bottom.
        float r = 5 * scale;
        d->AddRectFilledMultiColor(a, b, IM_COL32(52, 43, 36, 255), IM_COL32(52, 43, 36, 255), IM_COL32(24, 19, 16, 255),
                                   IM_COL32(24, 19, 16, 255));
        const LauncherImage *img = id > 0 ? art("fighters", id) : nullptr;
        if (img)
            cover(d, img, a, b, theirs ? IM_COL32(255, 255, 255, 90) : hovered ? IM_COL32_WHITE : IM_COL32(225, 225, 225, 255),
                  BUST, 0.0f, r);
        else if (id > 0)
            silhouette(d, ImVec2(a.x + 4 * scale, a.y + 4 * scale), ImVec2(b.x - 4 * scale, b.y - 22 * scale), name, scale);
        else
        {
            ImFont *f = pcui::display();
            ImVec2 ts = f->CalcTextSizeA(54 * scale, FLT_MAX, 0, "?");
            d->AddText(f, 54 * scale, ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - 20 * scale - ts.y) / 2),
                       ImGui::GetColorU32(pcui::amber()), "?");
        }
        fade(d, ImVec2(a.x, b.y - 34 * scale), b);
        centred(d, ImVec2(a.x + 2 * scale, b.y - 19 * scale), tile_w - 4 * scale, theirs ? SOFT : INK, name);
        d->AddRect(a, b, hovered ? IM_COL32(255, 255, 255, 120) : HAIR, r);
        if (theirs)
        {
            d->AddRectFilled(a, b, IM_COL32(0, 0, 0, 120), 5 * scale);
            const char *who = held == 99 ? "CPU" : player_label(l, held);
            centred(d, ImVec2(a.x, a.y + tile_h * 0.4f), tile_w, held == 99 ? SOFT : ui_player_colour(held), who);
        }
        // Every player's cursor, inset a little per player so they never hide each other.
        int inset = 0;
        for (int p = 0; p < NETPLAY_MAX_PLAYERS; p++)
        {
            if (!l.name[p][0])
                continue;
            int on = l.ready[p] && slot_of(l.match, p) >= 0 ? l.match.fighter[slot_of(l.match, p)] : sel.cursor[p];
            if (on != id || id <= 0)
                continue;
            float in = inset++ * 3 * scale;
            d->AddRect(ImVec2(a.x - 2 * scale + in, a.y - 2 * scale + in), ImVec2(b.x + 2 * scale - in, b.y + 2 * scale - in),
                       ui_player_colour(p), 6 * scale, 0, (l.ready[p] ? 3.5f : 2.0f) * scale);
            char tag[8];
            snprintf(tag, sizeof tag, "%s%s", p == me ? "You" : (std::string("P") + char('1' + p)).c_str(),
                     l.ready[p] ? " *" : "");
            ImVec2 ts = ImGui::CalcTextSize(tag);
            // The tag sits inside the tile, top left for P1 and P3, top right for P2 and P4.
            float tx = (p % 2) ? b.x - ts.x - 7 * scale - in : a.x + 6 * scale + in, ty = a.y + 5 * scale + in;
            d->AddRectFilled(ImVec2(tx - 3 * scale, ty - 1 * scale), ImVec2(tx + ts.x + 3 * scale, ty + ts.y + 1 * scale),
                             ui_player_colour(p), 3 * scale);
            d->AddText(ImVec2(tx, ty), GROUND, tag);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // The player cards.
    cards(l, sel, slots, fighters, count, width, cards_h, scale);

    // Lock in, change, created fighters; the host's way back.
    ImGui::Spacing();
    if (!locked)
    {
        int cur = me >= 0 ? sel.cursor[me] : 0;
        bool free = cur > 0 && cur < 1000 && (owner_of[cur] < 0 || owner_of[cur] == me);
        ImGui::BeginDisabled(!free || my_slot < 0);
        if (pcui::accent_button("LOCK IN", ImVec2(150 * scale, 34 * scale), 22 * scale, pcui::amber()))
        {
            netplay_pick(cur);
            netplay_set_ready(1);
        }
        ui_target("SelectLock");
        ImGui::EndDisabled();
        const std::vector<LauncherProfile> &profiles = launcher_profiles();
        if (!profiles.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button("Created fighter..."))
                ImGui::OpenPopup("created");
            if (ImGui::BeginPopup("created"))
            {
                for (const LauncherProfile &p : profiles)
                    if (ImGui::Selectable(p.name.c_str()))
                    {
                        netplay_pick_created(p.name.c_str());
                        netplay_set_ready(1);
                    }
                ImGui::EndPopup();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Arrow keys or mouse to move, Enter or click to lock in");
    }
    else
    {
        if (ImGui::Button("Change fighter"))
            netplay_set_ready(0);
        ui_target("SelectChange");
        ImGui::SameLine();
        ImGui::TextDisabled("Locked in. Waiting for the others...");
    }
    if (me == 0)
    {
        ImGui::SameLine();
        if (ImGui::Button("Back to the waiting room"))
            netplay_set_phase(NETPLAY_PHASE_WAITING);
    }
}

// ---- The venue vote: a map of New York ------------------------------------------------------
// The five boroughs, drawn plain (not to scale), in the map's own 0-1 coordinates.
struct Borough
{
    const char *name;
    ImVec2 label;
    std::vector<ImVec2> outline;
};
const std::vector<Borough> &boroughs()
{
    static const std::vector<Borough> b = {
        {"BRONX", {0.57f, 0.07f}, {{0.46f, 0.03f}, {0.66f, 0.02f}, {0.71f, 0.09f}, {0.68f, 0.18f}, {0.58f, 0.23f}, {0.50f, 0.21f}, {0.47f, 0.15f}}},
        {"MANHATTAN", {0.30f, 0.30f}, {{0.45f, 0.10f}, {0.47f, 0.14f}, {0.44f, 0.30f}, {0.41f, 0.45f}, {0.39f, 0.58f}, {0.36f, 0.63f}, {0.35f, 0.58f}, {0.37f, 0.44f}, {0.40f, 0.28f}, {0.43f, 0.13f}}},
        {"QUEENS", {0.78f, 0.42f}, {{0.48f, 0.31f}, {0.57f, 0.26f}, {0.72f, 0.25f}, {0.92f, 0.30f}, {0.96f, 0.50f}, {0.88f, 0.70f}, {0.72f, 0.73f}, {0.62f, 0.62f}, {0.54f, 0.53f}, {0.47f, 0.45f}}},
        {"BROOKLYN", {0.55f, 0.86f}, {{0.40f, 0.60f}, {0.46f, 0.49f}, {0.53f, 0.55f}, {0.62f, 0.65f}, {0.71f, 0.76f}, {0.65f, 0.88f}, {0.52f, 0.93f}, {0.41f, 0.87f}, {0.37f, 0.73f}}},
        {"STATEN ISLAND", {0.06f, 0.97f}, {{0.08f, 0.70f}, {0.19f, 0.62f}, {0.28f, 0.67f}, {0.27f, 0.84f}, {0.17f, 0.94f}, {0.07f, 0.88f}}},
    };
    return b;
}

// Where each venue is: by its name, in the borough its name or story puts it.
struct Pin
{
    const char *key;
    float x, y;
    int borough;        // index into boroughs()
};
const Pin PINS[] = {
    {"Hunt's Point Scrapyard After", 0.615f, 0.160f, 0}, {"Hunt's Point", 0.585f, 0.180f, 0},
    {"Gun Hill", 0.585f, 0.065f, 0}, {"Chopshop", 0.515f, 0.125f, 0},
    {"125th", 0.440f, 0.200f, 1}, {"Limit", 0.432f, 0.255f, 1}, {"Club 357 Intense", 0.428f, 0.315f, 1},
    {"Club 357", 0.412f, 0.300f, 1}, {"Club Murder", 0.398f, 0.350f, 1}, {"Crow's Office", 0.414f, 0.380f, 1},
    {"Red Room", 0.400f, 0.420f, 1}, {"7th Heaven Intense", 0.405f, 0.480f, 1}, {"7th Heaven", 0.392f, 0.470f, 1},
    {"Dragon House", 0.388f, 0.545f, 1}, {"The Pit", 0.372f, 0.590f, 1},
    {"Syn Energy", 0.505f, 0.365f, 2}, {"Heights", 0.660f, 0.400f, 2}, {"Foundation", 0.700f, 0.560f, 2},
    {"Terror Dome", 0.560f, 0.630f, 3}, {"Red Hook", 0.425f, 0.705f, 3}, {"Babylon", 0.570f, 0.760f, 3},
    {"Gauntlet Round", 0.495f, 0.825f, 3}, {"Gauntlet", 0.475f, 0.800f, 3},
    {"Stapleton", 0.235f, 0.705f, 4},
};

const Pin *pin_of(const char *venue_name)
{
    for (const Pin &p : PINS)
        if (venue_name && strstr(venue_name, p.key))
            return &p;
    return nullptr;
}

void ui_vote(const NetplayLobby &l, const NetplaySelect &sel, float width, float height, float scale)
{
    static int hover_venue = -1000;
    const NetplayChoice *fighters, *venues;
    int fcount = netplay_fighters(&fighters), vcount = netplay_venues(&venues);
    const NetplayMatchType *type = netplay_match_type(l.match.type);
    int slots = type ? type->fighters : 2, me = l.local_player;
    bool done = sel.result >= 0;
    ImDrawList *d = ImGui::GetWindowDrawList();
    ImVec2 top = ImGui::GetCursorScreenPos();
    const char *won_name = done ? netplay_choice_name(venues, vcount, sel.result) : nullptr;
    char kicker[96];
    if (done)
        snprintf(kicker, sizeof kicker, "%s. Loading the match...", won_name ? won_name : "Chosen");
    else
        snprintf(kicker, sizeof kicker, "Pick a spot on the map. Most votes wins; a tie is drawn by the host.");
    title(d, top, done ? "THE VENUE" : "CHOOSE THE VENUE", kicker, scale);
    clock(d, ImVec2(top.x + width, top.y), done ? -1 : sel.seconds_left, scale);
    ImGui::Dummy(ImVec2(width, 58 * scale));

    // Who fights.
    float cards_h = 80 * scale;
    cards(l, sel, slots, fighters, fcount, width, cards_h, scale);
    ImGui::Spacing();

    // How many votes each venue has, and whose.
    auto votes_on = [&](int venue) {
        int n = 0;
        for (int p = 0; p < NETPLAY_MAX_PLAYERS; p++)
            n += l.name[p][0] && sel.ballot[p] == venue;
        return n;
    };

    // What is left of the window below the cards, so the map never runs off its bottom.
    (void)height;
    float area_h = std::max(160 * scale, ImGui::GetContentRegionAvail().y - 10 * scale);
    float list_w = std::min(330 * scale, width * 0.34f), map_w = width - list_w - 10 * scale;
    ImVec2 ma = ImGui::GetCursorScreenPos(), mb(ma.x + map_w, ma.y + area_h);
    int hovered_now = -1000;

    // The map: water, the boroughs, their names.
    d->AddRectFilled(ma, mb, IM_COL32(16, 20, 26, 255), 8 * scale);
    d->AddRect(ma, mb, HAIR, 8 * scale);
    float side = std::min(map_w, area_h * 1.25f), mw = side, mh = side / 1.25f;
    ImVec2 o(ma.x + (map_w - mw) / 2, ma.y + (area_h - mh) / 2);
    auto at = [&](float x, float y) { return ImVec2(o.x + x * mw, o.y + y * mh); };
    for (const Borough &b : boroughs())
    {
        std::vector<ImVec2> pts;
        for (const ImVec2 &v : b.outline)
            pts.push_back(at(v.x, v.y));
        d->AddConcavePolyFilled(pts.data(), (int)pts.size(), IM_COL32(34, 29, 25, 255));
        d->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(217, 119, 43, 70), ImDrawFlags_Closed, 1.5f * scale);
        d->AddText(pcui::display(), 15 * scale, at(b.label.x, b.label.y), IM_COL32(200, 194, 186, 70), b.name);
    }

    // The pins: one per venue, with the votes beside it.
    ImGui::SetCursorScreenPos(ma);
    ImGui::InvisibleButton("map", ImVec2(map_w, area_h));
    bool map_hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;
    int nearest = -1;
    float nearest_d = 18 * scale;
    std::vector<int> unplaced;
    for (int i = 0; i < vcount; i++)
    {
        const Pin *pin = pin_of(venues[i].name);
        if (!pin)
        {
            unplaced.push_back(i);
            continue;
        }
        ImVec2 c = at(pin->x, pin->y);
        float dx = c.x - mouse.x, dy = c.y - mouse.y, dist = sqrtf(dx * dx + dy * dy);
        if (map_hovered && dist < nearest_d)
        {
            nearest_d = dist;
            nearest = i;
        }
    }
    if (nearest >= 0)
        hovered_now = venues[nearest].id;
    if (map_hovered && nearest >= 0 && ImGui::IsMouseClicked(0) && !done)
        netplay_vote_venue(venues[nearest].id);

    for (int i = 0; i < vcount; i++)
    {
        const Pin *pin = pin_of(venues[i].name);
        if (!pin)
            continue;
        int id = venues[i].id, n = votes_on(id);
        bool mine = me >= 0 && sel.ballot[me] == id, won = done && sel.result == id, lit = hover_venue == id;
        ImVec2 c = at(pin->x, pin->y);
        float r = (won ? 9.0f : lit || n ? 7.0f : 5.0f) * scale;
        ImU32 fill = won ? IM_COL32_WHITE : n ? ImGui::GetColorU32(pcui::amber()) : IM_COL32(200, 194, 186, 200);
        if (done && !won)
            fill = IM_COL32(120, 114, 106, 120);
        d->AddCircleFilled(c, r, fill);
        d->AddCircle(c, r + 2 * scale, mine ? ui_player_colour(me) : IM_COL32(0, 0, 0, 160), 0, (mine ? 2.5f : 1.0f) * scale);
        // Each voter's dot around the pin.
        int k = 0;
        for (int p = 0; p < NETPLAY_MAX_PLAYERS; p++)
            if (l.name[p][0] && sel.ballot[p] == id)
            {
                float ang = -1.57f + k++ * 0.9f;
                d->AddCircleFilled(ImVec2(c.x + cosf(ang) * (r + 8 * scale), c.y + sinf(ang) * (r + 8 * scale)), 4 * scale,
                                   ui_player_colour(p));
            }
        // A name where there is room: on the pin pointed at, voted for or chosen.
        if (lit || n || won)
        {
            ImVec2 ts = ImGui::CalcTextSize(venues[i].name);
            ImVec2 ta(c.x + 12 * scale, c.y - ts.y / 2);
            if (ta.x + ts.x > mb.x - 6 * scale)
                ta.x = c.x - 12 * scale - ts.x;
            d->AddRectFilled(ImVec2(ta.x - 4 * scale, ta.y - 2 * scale), ImVec2(ta.x + ts.x + 4 * scale, ta.y + ts.y + 2 * scale),
                             IM_COL32(11, 8, 6, 220), 4 * scale);
            d->AddText(ta, won ? IM_COL32_WHITE : INK, venues[i].name);
        }
    }
    // Beside the map: the venue pointed at (else the player's vote, else the winner), pictured.
    int shown = hover_venue != -1000 ? hover_venue : done ? sel.result : me >= 0 ? sel.ballot[me] : -1;
    if (done)
        shown = sel.result;
    float pic_h = list_w * 9 / 16;
    ImVec2 pa(mb.x + 10 * scale, ma.y), pb(pa.x + list_w, pa.y + pic_h);
    d->AddRectFilledMultiColor(pa, pb, IM_COL32(40, 33, 28, 255), IM_COL32(40, 33, 28, 255), IM_COL32(18, 14, 12, 255),
                               IM_COL32(18, 14, 12, 255));
    const char *shown_name = shown == NETPLAY_RANDOM_VENUE ? "Random venue"
                             : shown >= 0                  ? netplay_choice_name(venues, vcount, shown)
                                                           : "Point at a venue";
    if (const LauncherImage *img = shown >= 0 ? art("venues", shown) : nullptr)
        cover(d, img, pa, pb, IM_COL32_WHITE, WHOLE, 0.5f, 6 * scale);
    else
    {
        const char *mark = shown == NETPLAY_RANDOM_VENUE ? "?" : "NYC";
        ImVec2 ts = pcui::display()->CalcTextSizeA(44 * scale, FLT_MAX, 0, mark);
        d->AddText(pcui::display(), 44 * scale, ImVec2((pa.x + pb.x - ts.x) / 2, (pa.y + pb.y - ts.y) / 2 - 8 * scale),
                   IM_COL32(217, 119, 43, 90), mark);
    }
    fade(d, ImVec2(pa.x, pb.y - pic_h * 0.45f), pb, 240);
    {
        float size = 19 * scale;
        std::string s = shown_name ? shown_name : "?";
        while (s.size() > 3 && pcui::display()->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x > list_w - 20 * scale)
            s = s.substr(0, s.size() - 4) + "...";
        headline(d, size, ImVec2(pa.x + 10 * scale, pb.y - size - 10 * scale), done ? IM_COL32_WHITE : INK, s.c_str());
        int pin_borough = -1;
        if (shown >= 0)
            if (const Pin *p = pin_of(netplay_choice_name(venues, vcount, shown)))
                pin_borough = p->borough;
        if (pin_borough >= 0)
            d->AddText(ImVec2(pa.x + 10 * scale, pb.y - size - 12 * scale - ImGui::GetTextLineHeight()),
                       ImGui::GetColorU32(pcui::amber()), boroughs()[pin_borough].name);
    }
    d->AddRect(pa, pb, done ? IM_COL32_WHITE : HAIR, 6 * scale, 0, done ? 2 * scale : 1);

    // Under it, every venue by borough, and Random. Pointing at one lights its pin.
    ImGui::SetCursorScreenPos(ImVec2(pa.x, pb.y + 8 * scale));
    ImGui::BeginChild("venues", ImVec2(list_w, area_h - pic_h - 8 * scale), ImGuiChildFlags_NavFlattened, 0);
    auto row = [&](int id, const char *name) {
        int n = votes_on(id);
        bool mine = me >= 0 && sel.ballot[me] == id, won = done && sel.result == id;
        char label[96];
        snprintf(label, sizeof label, "%s##v%d", name, id);
        if (mine)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui_player_colour(me)));
        if (ImGui::Selectable(label, won || hover_venue == id, done ? ImGuiSelectableFlags_Disabled : 0) && !done)
            netplay_vote_venue(id);
        if (mine)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
            hovered_now = id;
        if (n)
        {
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            float x = b.x - 6 * scale;
            for (int p = NETPLAY_MAX_PLAYERS - 1; p >= 0; p--)
                if (l.name[p][0] && sel.ballot[p] == id)
                {
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(x, (a.y + b.y) / 2), 4 * scale, ui_player_colour(p));
                    x -= 11 * scale;
                }
        }
    };
    row(NETPLAY_RANDOM_VENUE, "Random venue");
    for (int bi = 0; bi < (int)boroughs().size(); bi++)
    {
        pcui::section(boroughs()[bi].name);
        for (int i = 0; i < vcount; i++)
        {
            const Pin *pin = pin_of(venues[i].name);
            if (pin && pin->borough == bi)
                row(venues[i].id, venues[i].name);
        }
    }
    if (!unplaced.empty())
    {
        pcui::section("ELSEWHERE");
        for (int i : unplaced)
            row(venues[i].id, venues[i].name);
    }
    ImGui::EndChild();
    hover_venue = hovered_now;
    ImGui::SetCursorScreenPos(ImVec2(ma.x, mb.y + 4 * scale));
    ImGui::Dummy(ImVec2(width, 1));
    ui_target("VoteVenue");
}
} // namespace

unsigned ui_player_colour(int player, int alpha)
{
    static const unsigned rgb[] = {0xd9772b, 0x5dcaa5, 0xed93b1, 0x85b7eb};
    unsigned c = rgb[(player < 0 ? 0 : player) % 4];
    return IM_COL32(c >> 16 & 255, c >> 8 & 255, c & 255, alpha);
}

void ui_online_select(const NetplayLobby &lobby, float width, float height, float scale)
{
    NetplaySelect sel;
    if (!netplay_select(&sel))
        return;
    float w = std::min(width, 1180 * scale), h = std::min(height, 720 * scale);
    ImGui::BeginChild("select", ImVec2(w, h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
    ImDrawList *d = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetWindowPos();
    // The ground: near black, warmed by a glow of amber from the top.
    d->AddRectFilled(a, ImVec2(a.x + w, a.y + h), GROUND, 8 * scale);
    d->AddRectFilledMultiColor(a, ImVec2(a.x + w, a.y + h * 0.5f), IM_COL32(217, 119, 43, 34), IM_COL32(217, 119, 43, 10),
                               IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    d->AddRect(a, ImVec2(a.x + w, a.y + h), HAIR, 8 * scale);
    ImGui::SetCursorPos(ImVec2(14 * scale, 12 * scale));
    ImGui::BeginGroup();
    float iw = w - 28 * scale, ih = h - 24 * scale;
    if (lobby.phase == NETPLAY_PHASE_VOTING)
        ui_vote(lobby, sel, iw, ih, scale);
    else
        ui_pick(lobby, sel, iw, ih, scale);
    ImGui::EndGroup();
    ImGui::EndChild();
}
