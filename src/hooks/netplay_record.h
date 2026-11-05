#ifndef DEFJAM_NETPLAY_RECORD_H
#define DEFJAM_NETPLAY_RECORD_H
/* The player's online record (src/hooks/netplay_record.c, docs/netplay.md): an ID made once
 * per copy, every online match's result in <data>/online/history.txt, and what the launcher
 * shows from it: the record, a rank title, the favourite fighter, the rivals. Online matches
 * only; netplay.c adds the matches, and their replays go to <data>/online/replays. */
#include <stddef.h>
#include "netplay.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_ID_LEN       17      /* 16 hex digits */
#define ONLINE_OPPONENTS    3
#define ONLINE_HISTORY_MAX  1000    /* matches read back, newest first */

typedef struct {
    long long when;                         /* seconds since 1970, when it started */
    char result;                            /* 'W', 'L', 'D', or 'U' unfinished (a player left) */
    int counted;                            /* in the record: finished, against another player */
    int type, venue;                        /* Game.SetMatchType, ChooseVenue */
    int fighter;                            /* this player's (NETPLAY_CREATED_FIGHTER: profile's) */
    char profile[NETPLAY_PROFILE_LEN];
    char name[NETPLAY_NAME_LEN];            /* this player's online name then */
    char replay[64];                        /* its file in replays/, "" none */
    int opponents;                          /* the other players against this one (not the CPU) */
    char opponent[ONLINE_OPPONENTS][NETPLAY_NAME_LEN];
    char opponent_id[ONLINE_OPPONENTS][ONLINE_ID_LEN];
} OnlineMatchRecord;

typedef struct {
    int matches, wins, losses, draws;       /* counted ones */
    int streak;                             /* the current: +3 three wins, -2 two losses */
    int best_streak;                        /* the longest run of wins */
    int favourite;                          /* most played fighter, -1 none */
    char favourite_profile[NETPLAY_PROFILE_LEN];
    int favourite_matches;
    int best_venue, best_venue_wins, best_venue_losses;     /* -1 none */
} OnlineStats;

typedef struct {
    char id[ONLINE_ID_LEN];
    char name[NETPLAY_NAME_LEN];            /* the name they last played under */
    int matches, wins, losses, draws;       /* counted, this player's view */
    int best_streak;                        /* this player's longest run of wins against them */
    long long last;
} OnlineRival;

/* <data>/online (DEFJAM_DATA/online, else beside the default saves), made if missing. */
int online_dir(char *out, size_t size);
/* This copy's player ID, made once and kept in <data>/online/player.ini. */
const char *online_player_id(void);
/* An ID as it travels: 16 hex digits, or "" (anything else). */
void online_clean_id(char *out, const char *in);

/* Append a match to history.txt. */
int online_record_add(const OnlineMatchRecord *r);
/* The history, newest first; returns how many. One line a match, so a line cut short by a
 * crash loses that match only. */
int online_history(OnlineMatchRecord *out, int max);
/* The history file's one line for `r`, and back (0: not a match line). */
void online_record_line(const OnlineMatchRecord *r, char *out, size_t size);
int online_record_parse(const char *line, OnlineMatchRecord *r);

void online_stats(const OnlineMatchRecord *history, int count, OnlineStats *out);
/* Every player met, most matches first. */
int online_rivals(const OnlineMatchRecord *history, int count, OnlineRival *out, int max);
/* The rank title for so many counted wins. */
const char *online_title(int wins);

/* How many replays are kept: beginning a new one deletes the oldest beyond `keep` in `dir`
 * (their names start with the time, so name order is age order). */
#define ONLINE_REPLAYS_KEPT 100
void online_prune_replays(const char *dir, int keep);

#ifdef __cplusplus
}
#endif
#endif
