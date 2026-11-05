// The player's online record in the launcher (pc_launcher_online.h): read from
// <data>/online (netplay_record.c), shown on the Online Match page, with the match history and
// the rivals in windows of their own.
#include "pc_launcher_online.h"
#include "pc_launcher_profiles.h"
#include "pc_ui_internal.h"
#include "pc_ui_theme.h"
#include "netplay.h"
#include "netplay_record.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
struct Replay
{
    bool exists = false, playable = false;
    std::string why;            // what it is, or why this build cannot play it
};

struct Record
{
    std::vector<OnlineMatchRecord> history;     // newest first
    OnlineStats stats{};
    std::vector<OnlineRival> rivals;
    std::map<std::string, Replay> replays;      // by file name, checked once each
    double loaded = -100;
} g;

std::string replay_path(const char *file)
{
    char dir[1024];
    if (!file[0] || !online_dir(dir, sizeof dir))
        return "";
    // A narrow path, as netplay.c opens it (fopen) and online_dir made it (getenv).
#if defined(_WIN32)
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    return std::string(dir) + sep + "replays" + sep + file;
}

void load(bool now)
{
    if (!now && ImGui::GetTime() - g.loaded < 3)
        return;
    g.loaded = ImGui::GetTime();
    g.history.resize(ONLINE_HISTORY_MAX);
    g.history.resize((size_t)online_history(g.history.data(), ONLINE_HISTORY_MAX));
    online_stats(g.history.data(), (int)g.history.size(), &g.stats);
    g.rivals.resize(64);
    g.rivals.resize((size_t)online_rivals(g.history.data(), (int)g.history.size(), g.rivals.data(), 64));
    for (const OnlineMatchRecord &r : g.history)
    {
        if (!r.replay[0])
            continue;
        std::string path = replay_path(r.replay);
        std::error_code error;
        bool exists = !path.empty() && std::filesystem::is_regular_file(std::filesystem::path(path), error);
        auto found = g.replays.find(r.replay);
        if (found != g.replays.end() && found->second.exists == exists)
            continue;
        Replay replay;
        replay.exists = exists;
        if (exists)
        {
            char why[160];
            replay.playable = netplay_replay_check(path.c_str(), why, sizeof why) != 0;
            replay.why = why;
        }
        else
            replay.why = "The replay was removed (the newest " + std::to_string(ONLINE_REPLAYS_KEPT) + " are kept)";
        g.replays[r.replay] = replay;
    }
}

std::string fighter_name(int fighter, const char *profile)
{
    if (fighter == NETPLAY_CREATED_FIGHTER)
        return profile[0] ? std::string(profile) + " (created)" : "a created fighter";
    const NetplayChoice *list;
    int count = netplay_fighters(&list);
    const char *name = netplay_choice_name(list, count, fighter);
    return name ? name : "fighter " + std::to_string(fighter);
}

std::string venue_name(int venue)
{
    const NetplayChoice *list;
    int count = netplay_venues(&list);
    const char *name = netplay_choice_name(list, count, venue);
    return name ? name : "venue " + std::to_string(venue);
}

std::string when_text(long long when)
{
    std::string day = launcher_played_text(when);
    time_t t = (time_t)when;
    struct tm local;
#if defined(_WIN32)
    if (localtime_s(&local, &t) != 0)
        return day;
#else
    if (!localtime_r(&t, &local))
        return day;
#endif
    char clock[16];
    strftime(clock, sizeof clock, "%H:%M", &local);
    if (day == "today" || day == "tonight" || day == "yesterday")
        return (day == "yesterday" ? "Yesterday " : "Today ") + std::string(clock);
    return day;
}

std::string opponents_text(const OnlineMatchRecord &r)
{
    if (!r.opponents)
        return "the CPU";
    std::string out = r.opponent[0];
    for (int i = 1; i < r.opponents; i++)
        out += (i + 1 == r.opponents ? " and " : ", ") + std::string(r.opponent[i]);
    return out;
}

std::string record_text(int wins, int losses, int draws)
{
    std::string out = std::to_string(wins) + "-" + std::to_string(losses);
    if (draws)
        out += "-" + std::to_string(draws);
    return out;
}

const ImVec4 WIN(0.42f, 0.78f, 0.36f, 1), LOSS(0.90f, 0.36f, 0.30f, 1), PLAIN(0.70f, 0.67f, 0.62f, 1);

// One figure: its label small and dim over it, the figure large.
void figure(const char *label, const std::string &value, float s)
{
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", label);
    ImDrawList *d = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    d->AddText(pcui::display(), 26 * s, at, IM_COL32_WHITE, value.c_str());
    ImGui::Dummy(ImVec2(std::max(90 * s, pcui::display()->CalcTextSizeA(26 * s, FLT_MAX, 0, value.c_str()).x), 28 * s));
    ImGui::EndGroup();
}

void history_window(UiResult &result, float width, float height, float s)
{
    ImGui::SetNextWindowSizeConstraints(ImVec2(520 * s, 0), ImVec2(width - 40 * s, height - 40 * s));
    if (!ImGui::BeginPopupModal("Match history##online", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (ImGui::IsWindowAppearing())
        load(true);
    ImGui::TextDisabled("Every online match, newest first. A match a player left is listed but not counted.");
    ImGui::Spacing();
    if (g.history.empty())
        ImGui::TextWrapped("No online matches yet. Host or join a lobby to play one.");
    else if (ImGui::BeginTable("history", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                               ImVec2(std::min(760 * s, width - 80 * s), std::min(420 * s, height - 200 * s))))
    {
        ImGui::TableSetupColumn("Result");
        ImGui::TableSetupColumn("Match", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("When");
        ImGui::TableSetupColumn("Replay");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        int row = 0;
        for (const OnlineMatchRecord &r : g.history)
        {
            ImGui::PushID(row++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const char *word = r.result == 'W' ? "Win" : r.result == 'L' ? "Loss" : r.result == 'D' ? "Draw" : "Left";
            ImVec4 colour = r.result == 'W' ? WIN : r.result == 'L' ? LOSS : PLAIN;
            ImGui::TextColored(colour, "%s", word);
            ImGui::TableNextColumn();
            ImGui::Text("vs %s", opponents_text(r).c_str());
            const NetplayMatchType *type = netplay_match_type(r.type);
            std::string detail = venue_name(r.venue) + " \xC2\xB7 " + (type ? type->name : "match") + " \xC2\xB7 as " +
                                 fighter_name(r.fighter, r.profile);
            if (!r.counted)
                detail += r.result == 'U' ? " \xC2\xB7 not counted" : " \xC2\xB7 practice, not counted";
            ImGui::TextDisabled("%s", detail.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(when_text(r.when).c_str());
            ImGui::TableNextColumn();
            auto replay = r.replay[0] ? g.replays.find(r.replay) : g.replays.end();
            if (replay == g.replays.end())
                ImGui::TextDisabled("No replay");
            else
            {
                ImGui::BeginDisabled(!replay->second.playable);
                if (ImGui::Button("Watch"))
                {
                    netplay_replay_choose(replay_path(r.replay).c_str());
                    result.play = true;
                    result.start = 0;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", replay->second.why.c_str());
                if (row == 1)
                    ui_target("WatchNewest");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    bool back = !ImGui::IsWindowAppearing() &&
                (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
    if (ImGui::Button("Close") || back)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void rivals_window(float width, float height, float s)
{
    ImGui::SetNextWindowSizeConstraints(ImVec2(440 * s, 0), ImVec2(width - 40 * s, height - 40 * s));
    if (!ImGui::BeginPopupModal("Rivals##online", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (ImGui::IsWindowAppearing())
        load(true);
    ImGui::TextDisabled("Your record against everyone you have played online.");
    ImGui::Spacing();
    if (g.rivals.empty())
        ImGui::TextWrapped("Nobody yet. Every player you fight online shows up here, with your record against them.");
    ImGui::BeginChild("rivals", ImVec2(std::min(560 * s, width - 80 * s), std::min(84 * s * (float)std::max<size_t>(1, g.rivals.size()), std::min(420 * s, height - 200 * s))));
    for (const OnlineRival &r : g.rivals)
    {
        ImGui::PushID(r.id);
        ImDrawList *d = ImGui::GetWindowDrawList();
        ImVec2 at = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        d->AddText(pcui::display(), 22 * s, at, IM_COL32_WHITE, r.name);
        std::string score = record_text(r.wins, r.losses, r.draws);
        float sw = pcui::display()->CalcTextSizeA(22 * s, FLT_MAX, 0, score.c_str()).x;
        d->AddText(pcui::display(), 22 * s, ImVec2(at.x + w - sw, at.y), IM_COL32_WHITE, score.c_str());
        ImGui::Dummy(ImVec2(w, 26 * s));
        // their share against yours: green for yours, red for theirs
        ImVec2 bar = ImGui::GetCursorScreenPos();
        float decided = (float)(r.wins + r.losses), share = decided > 0 ? r.wins / decided : 0.5f;
        d->AddRectFilled(bar, ImVec2(bar.x + w, bar.y + 6 * s), ImGui::ColorConvertFloat4ToU32(LOSS), 3 * s);
        if (share > 0)
            d->AddRectFilled(bar, ImVec2(bar.x + w * share, bar.y + 6 * s), ImGui::ColorConvertFloat4ToU32(WIN), 3 * s);
        ImGui::Dummy(ImVec2(w, 10 * s));
        std::string line = "Last played " + launcher_played_text(r.last) + " \xC2\xB7 " + std::to_string(r.matches) +
                           (r.matches == 1 ? " match" : " matches");
        if (r.matches)
            line += " \xC2\xB7 you " + std::to_string((int)(share * 100 + 0.5f)) + "%";
        if (r.best_streak > 1)
            line += " \xC2\xB7 your best run " + std::to_string(r.best_streak) + " wins";
        ImGui::TextDisabled("%s", line.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();
    bool back = !ImGui::IsWindowAppearing() &&
                (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
    if (ImGui::Button("Close") || back)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
} // namespace

void ui_online_record(UiResult &result, const char *name, float width, float height, float s)
{
    load(false);
    const OnlineStats &st = g.stats;
    pcui::section("Your online record");
    ImDrawList *d = ImGui::GetWindowDrawList();
    ImVec2 at = ImGui::GetCursorScreenPos();
    const char *title = online_title(st.wins);
    d->AddText(pcui::display(), 26 * s, at, IM_COL32_WHITE, name && *name ? name : "Player");
    float nw = pcui::display()->CalcTextSizeA(26 * s, FLT_MAX, 0, name && *name ? name : "Player").x;
    d->AddText(pcui::display(), 20 * s, ImVec2(at.x + nw + 14 * s, at.y + 5 * s),
               ImGui::ColorConvertFloat4ToU32(pcui::amber()), title);
    ImGui::Dummy(ImVec2(nw + 200 * s, 30 * s));
    if (!st.matches && g.history.empty())
        ImGui::TextWrapped("No online matches yet. Your wins, losses, rivals and the replay of every match will show here.");
    else
    {
        figure("Record", record_text(st.wins, st.losses, st.draws), s);
        ImGui::SameLine(0, 28 * s);
        int decided = st.wins + st.losses;
        figure("Win rate", decided ? std::to_string((st.wins * 100 + decided / 2) / decided) + "%" : "-", s);
        ImGui::SameLine(0, 28 * s);
        figure("Streak", st.streak > 0 ? "W" + std::to_string(st.streak) : st.streak < 0 ? "L" + std::to_string(-st.streak) : "-", s);
        ImGui::SameLine(0, 28 * s);
        figure("Matches", std::to_string(st.matches), s);
        std::string line;
        if (st.favourite >= 0)
            line = "Favourite fighter: " + fighter_name(st.favourite, st.favourite_profile) + " (" +
                   std::to_string(st.favourite_matches) + (st.favourite_matches == 1 ? " match)" : " matches)");
        if (st.best_venue >= 0)
            line += (line.empty() ? "" : "   \xC2\xB7   ") + std::string("Best venue: ") + venue_name(st.best_venue) + " " +
                    record_text(st.best_venue_wins, st.best_venue_losses, 0);
        if (st.best_streak > 1)
            line += (line.empty() ? "" : "   \xC2\xB7   ") + std::string("Best run: ") + std::to_string(st.best_streak) + " wins";
        if (!line.empty())
            ImGui::TextDisabled("%s", line.c_str());
    }
    if (ImGui::Button(("Match history (" + std::to_string(g.history.size()) + ")").c_str()))
        ImGui::OpenPopup("Match history##online");
    ui_target("OnlineHistory");
    ImGui::SameLine();
    if (ImGui::Button(("Rivals (" + std::to_string(g.rivals.size()) + ")").c_str()))
        ImGui::OpenPopup("Rivals##online");
    ui_target("OnlineRivals");
    history_window(result, width, height, s);
    rivals_window(width, height, s);
}

std::string ui_online_summary()
{
    load(false);
    if (!g.stats.matches)
        return "";
    return record_text(g.stats.wins, g.stats.losses, g.stats.draws) + ", " + online_title(g.stats.wins);
}
