/*
 * What an online match can be: the match types of Battle's Choose Match Type screen,
 * and the fighters and venues, read from the game's own fighter.xml and venue.xml so
 * that the names are the dump's (docs/netplay.md).
 *
 * The match-type numbers are what the screen hands Game.SetMatchType, read from runs
 * that chose each one (2026-10-29). The special types choose their own venue
 * (Game.ChooseVenue right after SetMatchType), so the host picks a venue only for the
 * first three, which also have the rules screen (Game.SetMatchRules).
 *
 * And the player's own profiles, for their created fighters: the save folders, one per
 * save, under UserData/45410049 in the folder main.c gives the saves.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#define SEP "\\"
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#define SEP "/"
#define _stricmp strcasecmp
#endif

#include "netplay.h"

static const NetplayMatchType s_types[] = {
    {0, "One on One", 2, -1, 1},
    {1, "Team Match", 4, -1, 1},
    {2, "Free For All", 4, -1, 1},
    {6, "Demolition", 2, 17, 0},
    {7, "Inferno", 2, 24, 0},
    {8, "Subway", 2, 11, 0},
    {9, "Window", 2, 26, 0},
    {10, "Cage", 2, 10, 0},
    {11, "Ring Out", 2, 8, 0},
};

int netplay_match_types(const NetplayMatchType **out)
{
    *out = s_types;
    return (int)(sizeof s_types / sizeof s_types[0]);
}

const NetplayMatchType *netplay_match_type(int type)
{
    size_t i;
    for (i = 0; i < sizeof s_types / sizeof s_types[0]; i++)
        if (s_types[i].type == type) return &s_types[i];
    return NULL;
}

/* ---- fighter.xml and venue.xml ------------------------------------------------------ */
#define MAX_CHOICES 160
static NetplayChoice s_fighters[MAX_CHOICES], s_venues[MAX_CHOICES];
static int s_fighter_count = -1, s_venue_count = -1;

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *text;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char *)malloc((size_t)n + 1);
    if (text && fread(text, 1, (size_t)n, f) != (size_t)n) { free(text); text = NULL; }
    if (text) text[n] = 0;
    fclose(f);
    return text;
}

/* The text of <tag>...</tag> inside [start, end), or "" when absent. */
static void field(const char *start, const char *end, const char *tag, char *out, size_t size)
{
    char open[32], close[32];
    const char *a, *b;
    size_t n;
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    out[0] = 0;
    a = strstr(start, open);
    if (!a || a >= end) return;
    a += strlen(open);
    b = strstr(a, close);
    if (!b || b > end) return;
    n = (size_t)(b - a);
    if (n >= size) n = size - 1;
    memcpy(out, a, n);
    out[n] = 0;
}

/* Fighting styles as fighter.xml abbreviates them after the name. */
static const char *style_name(const char *code)
{
    static const char *map[][2] = {{"STF", "Street"}, {"MAR", "Martial Arts"}, {"KIC", "Kickboxing"},
                                   {"WRE", "Wrestling"}, {"SUB", "Submissions"}};
    size_t i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(code, map[i][0])) return map[i][1];
    return NULL;
}

static void load_fighters(void)
{
    char *text = read_file("game/fighter.xml"), *p;
    s_fighter_count = 0;
    if (!text) return;
    for (p = strstr(text, "<fighter>"); p && s_fighter_count < MAX_CHOICES; p = strstr(p + 1, "<fighter>")) {
        char *end = strstr(p, "</fighter>"), id[16], name[64], disp[64], *paren;
        NetplayChoice *c;
        if (!end) break;
        field(p, end, "ID", id, sizeof id);
        field(p, end, "name", name, sizeof name);
        field(p, end, "dispname", disp, sizeof disp);
        /* Placeholders, the created-player parts (CP ...) and the story-only Crow. */
        if (!id[0] || !disp[0] || strstr(disp, "EMPTY") || strstr(disp, "NO PLAYABLE")
            || !strncmp(disp, "CP", 2) || !strncmp(name, "---", 3) || !strcmp(name, "LOCKED"))
            continue;
        c = &s_fighters[s_fighter_count++];
        c->id = atoi(id);
        paren = strchr(disp, '(');
        if (paren) {
            char code[8] = "";
            const char *style;
            sscanf(paren + 1, "%7[A-Z]", code);
            style = style_name(code);
            while (paren > disp && paren[-1] == ' ') paren--;
            *paren = 0;
            if (style) snprintf(c->name, sizeof c->name, "%s (%s)", disp, style);
            else snprintf(c->name, sizeof c->name, "%s", disp);
        } else {
            snprintf(c->name, sizeof c->name, "%s", disp);
        }
    }
    free(text);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const NetplayChoice *)a)->name, ((const NetplayChoice *)b)->name);
}

static void load_venues(void)
{
    char *text = read_file("game/venue.xml"), *p;
    s_venue_count = 0;
    if (!text) return;
    for (p = strstr(text, "<venue>"); p && s_venue_count < MAX_CHOICES; p = strstr(p + 1, "<venue>")) {
        char *end = strstr(p, "</venue>"), name[32], disp[96], fe[16], *paren, *shown;
        int id, i, seen = 0;
        if (!end) break;
        field(p, end, "name", name, sizeof name);
        field(p, end, "dispname", disp, sizeof disp);
        field(p, end, "feID", fe, sizeof fe);
        /* Arenas only (not the Story rooms), not the cut ones, the viewer or the
         * cutscene-only alley; one entry per ChooseVenue number. */
        if (strncmp(name, "ARENA_", 6) || !fe[0] || strstr(disp, "CUT") || strstr(disp, "Viewer")
            || strstr(disp, "Cinema"))
            continue;
        id = atoi(fe);
        for (i = 0; i < s_venue_count; i++) seen |= s_venues[i].id == id;
        if (seen || id < 0) continue;
        /* "02A The Foundation  (FightClub)" -> "The Foundation" */
        shown = strchr(disp, ' ');
        shown = shown ? shown + 1 : disp;
        paren = strchr(shown, '(');
        if (paren) *paren = 0;
        for (i = (int)strlen(shown); i > 0 && shown[i - 1] == ' '; i--) shown[i - 1] = 0;
        s_venues[s_venue_count].id = id;
        snprintf(s_venues[s_venue_count].name, sizeof s_venues[0].name, "%s", shown);
        s_venue_count++;
    }
    free(text);
    qsort(s_venues, (size_t)s_venue_count, sizeof s_venues[0], by_name);
}

int netplay_fighters(const NetplayChoice **out)
{
    if (s_fighter_count < 0) {
        load_fighters();
        qsort(s_fighters, (size_t)s_fighter_count, sizeof s_fighters[0], by_name);
    }
    *out = s_fighters;
    return s_fighter_count;
}

int netplay_venues(const NetplayChoice **out)
{
    if (s_venue_count < 0) load_venues();
    *out = s_venues;
    return s_venue_count;
}

const char *netplay_choice_name(const NetplayChoice *list, int count, int id)
{
    int i;
    for (i = 0; i < count; i++)
        if (list[i].id == id) return list[i].name;
    return NULL;
}

/* ---- Saves ------------------------------------------------------------------------- */
static int by_text(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int netplay_list_dir(const char *dir, int dirs, char names[][64], int max)
{
    int n = 0;
#if defined(_WIN32)
    char pattern[1100];
    WIN32_FIND_DATAA f;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        int is_dir = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (f.cFileName[0] == '.' || is_dir != !!dirs || strlen(f.cFileName) >= 64) continue;
        snprintf(names[n++], 64, "%s", f.cFileName);
    } while (n < max && FindNextFileA(h, &f));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return 0;
    while (n < max && (e = readdir(d)) != NULL) {
        char path[1100];
        struct stat st;
        if (e->d_name[0] == '.' || strlen(e->d_name) >= 64) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) != !!dirs) continue;
        snprintf(names[n++], 64, "%s", e->d_name);
    }
    closedir(d);
#endif
    qsort(names, (size_t)n, sizeof names[0], by_text);
    return n;
}

/* main.c's rule: DEFJAM_DATA/save, or the runtime's own default. */
int netplay_save_root(char *out, int size)
{
    const char *data = getenv("DEFJAM_DATA");
    if (data && *data) {
        size_t n = strlen(data);
        snprintf(out, (size_t)size, "%s%ssave", data, data[n - 1] == SEP[0] ? "" : SEP);
        return 1;
    }
#if defined(_WIN32)
    data = getenv("LOCALAPPDATA");
    if (!data || !*data) return 0;
    snprintf(out, (size_t)size, "%s\\xboxrecomp", data);
#else
    data = getenv("XDG_DATA_HOME");
    if (data && *data) snprintf(out, (size_t)size, "%s/xboxrecomp", data);
    else if ((data = getenv("HOME")) != NULL) snprintf(out, (size_t)size, "%s/.local/share/xboxrecomp", data);
    else return 0;
#endif
    return 1;
}

/* A save's own file: not the dashboard's metadata or picture. */
int netplay_save_file(const char *name)
{
    return _stricmp(name, "SaveMeta.xbx") && _stricmp(name, "saveimage.xbx");
}

/* The save `name` (a profile, or "Options") in `root`'s UserData: its folder. */
int netplay_save_folder(const char *root, const char *name, char folder[64])
{
    char dir[1024], path[1100], folders[64][64], files[8][64];
    int n, i, j, k;
    snprintf(dir, sizeof dir, "%s" SEP "UserData" SEP "45410049", root);
    n = netplay_list_dir(dir, 1, folders, 64);
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s" SEP "%s", dir, folders[i]);
        k = netplay_list_dir(path, 0, files, 8);
        for (j = 0; j < k; j++)
            if (!strcmp(files[j], name)) { snprintf(folder, 64, "%s", folders[i]); return 1; }
    }
    return 0;
}

int netplay_profiles(char names[][NETPLAY_PROFILE_LEN], int max)
{
    char root[1024], dir[1100], path[1200], folders[64][64], files[8][64];
    int n, i, j, k, count = 0;
    if (!netplay_save_root(root, sizeof root)) return 0;
    snprintf(dir, sizeof dir, "%s" SEP "UserData" SEP "45410049", root);
    n = netplay_list_dir(dir, 1, folders, 64);
    for (i = 0; i < n && count < max; i++) {
        snprintf(path, sizeof path, "%s" SEP "%s", dir, folders[i]);
        k = netplay_list_dir(path, 0, files, 8);
        for (j = 0; j < k && count < max; j++)
            if (netplay_save_file(files[j]) && strcmp(files[j], "Options") && strlen(files[j]) < NETPLAY_PROFILE_LEN)
                snprintf(names[count++], NETPLAY_PROFILE_LEN, "%s", files[j]);
    }
    return count;
}
