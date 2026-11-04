#pragma once
#include "pc_launcher_art.h"
#include <string>
#include <vector>

// The player's profiles as the launcher's Home shows them, read from the save folder once per
// ImGui context (the launcher shows once, before the game, so nothing changes them meanwhile).
struct LauncherProfile
{
    std::string name;
    int user = 0;                           // its place below CREATE USER ID on the game's list
    int story = -1;                         // Story completed in percent, as the game shows it; -1 unread
    unsigned cash = 0;
    unsigned story_seconds = 0;             // time played in Story
    long long played = 0;                   // the save's last write, seconds since 1970
    const LauncherImage *fighter = nullptr; // the created fighter, full length (128 px square)
    const LauncherImage *face = nullptr;    // and the head (32 px)
};

const std::vector<LauncherProfile> &launcher_profiles();

// Whose story Continue picks up: the [launcher] profile setting when it names one of them, else
// the most recently played. nullptr when there are none.
const LauncherProfile *launcher_current_profile();

// When `played` was, in a word or two: "tonight", "yesterday", "3 days ago", "12 Oct".
std::string launcher_played_text(long long played);
