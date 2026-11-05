#pragma once
struct UiResult;

// The player's online record on the launcher's Online Match page (netplay_record.h): who they
// are online, their record and rank title, and the buttons to the match history (with each
// match's replay) and the rivals, each in a window over the page. `name` is the online name
// the page has. Watch on a replay sets `result.play`, with the replay chosen
// (netplay_replay_choose): the game starts straight into it.
void ui_online_record(UiResult &result, const char *name, float width, float height, float scale);

#include <string>
// The record in a few words for Home's Online tile ("12-4, Contender"), "" before any counted
// match.
std::string ui_online_summary();
