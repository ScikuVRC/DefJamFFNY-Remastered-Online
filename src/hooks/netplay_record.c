/* The player's online record (netplay_record.h). Plain files under <data>/online:
 *
 *   player.ini     id=<16 hex>: this copy's player, made once; other games keep their
 *                  record against this ID, so a new name does not split it
 *   history.txt    one line per online match, tab-separated (online_record_line)
 *   replays/       each match's inputs (netplay.c)
 *
 * Nothing here touches the game or its saves: the match results come from netplay.c. */
#define _CRT_SECURE_NO_WARNINGS
#if defined(_WIN32)
#define _CRT_RAND_S
#include <direct.h>
#include <windows.h>
#define make_dir(path) _mkdir(path)
#define SEP "\\"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define make_dir(path) mkdir(path, 0777)
#define SEP "/"
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "netplay_record.h"

int online_dir(char *out, size_t size)
{
    const char *data = getenv("DEFJAM_DATA");
    if (data && *data) {
        size_t n = strlen(data);
        snprintf(out, size, "%s%sonline", data, data[n - 1] == SEP[0] || data[n - 1] == '/' ? "" : SEP);
    } else {
#if defined(_WIN32)
        data = getenv("LOCALAPPDATA");
        if (!data || !*data) return 0;
        snprintf(out, size, "%s\\xboxrecomp\\online", data);
#else
        if ((data = getenv("XDG_DATA_HOME")) != NULL && *data) snprintf(out, size, "%s/xboxrecomp/online", data);
        else if ((data = getenv("HOME")) != NULL) snprintf(out, size, "%s/.local/share/xboxrecomp/online", data);
        else return 0;
#endif
    }
    make_dir(out);
    return 1;
}

void online_clean_id(char *out, const char *in)
{
    int i;
    for (i = 0; in && i < ONLINE_ID_LEN - 1 && isxdigit((unsigned char)in[i]); i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    /* exactly 16 hex digits, or nothing */
    if (i != ONLINE_ID_LEN - 1 || isxdigit((unsigned char)in[i])) i = 0;
    out[i] = 0;
}

static unsigned long long random64(void)
{
    unsigned long long v = 0;
#if defined(_WIN32)
    unsigned int a = 0, b = 0;
    if (rand_s(&a) == 0 && rand_s(&b) == 0) return (unsigned long long)a << 32 | b;
    v = (unsigned long long)GetTickCount64() * 6364136223846793005ull ^ (unsigned long long)GetCurrentProcessId();
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(&v, sizeof v, 1, f);
        fclose(f);
        if (got == 1) return v;
    }
    v = (unsigned long long)getpid() * 6364136223846793005ull;
#endif
    return v ^ (unsigned long long)time(NULL) * 1442695040888963407ull;
}

const char *online_player_id(void)
{
    static char id[ONLINE_ID_LEN];
    char dir[1024], path[1100], line[128];
    FILE *f;
    if (id[0]) return id;
    if (!online_dir(dir, sizeof dir)) return id;
    snprintf(path, sizeof path, "%s" SEP "player.ini", dir);
    if ((f = fopen(path, "r")) != NULL) {
        while (!id[0] && fgets(line, sizeof line, f))
            if (!strncmp(line, "id=", 3)) online_clean_id(id, line + 3);
        fclose(f);
    }
    if (!id[0]) {
        snprintf(id, sizeof id, "%016llx", random64());
        if ((f = fopen(path, "w")) != NULL) {
            fprintf(f, "id=%s\n", id);
            fclose(f);
        }
    }
    return id;
}

/* ---- history.txt --------------------------------------------------------------- */
static void hex_text(char *out, size_t size, const char *in)
{
    size_t n = 0;
    if (!*in) { snprintf(out, size, "-"); return; }
    for (; *in && n + 3 <= size; in++, n += 2) snprintf(out + n, 3, "%02x", (unsigned char)*in);
    out[n] = 0;
}

static void unhex_text(char *out, size_t size, const char *in)
{
    size_t n = 0;
    unsigned v;
    if (strcmp(in, "-"))
        for (; in[0] && in[1] && n + 1 < size && sscanf(in, "%2x", &v) == 1; in += 2) out[n++] = (char)v;
    out[n] = 0;
}

/* A name in the file: letters, digits, '-' and '_' (as netplay.c sends them). */
static void plain(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; in && *in && n + 1 < size; in++)
        out[n++] = isalnum((unsigned char)*in) || *in == '-' ? *in : '_';
    if (!n && size > 1) out[n++] = '?';
    out[n] = 0;
}

void online_record_line(const OnlineMatchRecord *r, char *out, size_t size)
{
    char profile[2 * NETPLAY_PROFILE_LEN + 1], name[NETPLAY_NAME_LEN], replay[64];
    size_t n;
    int i;
    hex_text(profile, sizeof profile, r->profile);
    plain(name, sizeof name, r->name);
    snprintf(replay, sizeof replay, "%s",
             r->replay[0] && !strpbrk(r->replay, "\t\r\n/\\:") && r->replay[0] != '.' ? r->replay : "-");
    n = (size_t)snprintf(out, size, "1\t%lld\t%c\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%d", r->when, r->result, r->counted ? 1 : 0,
                         r->type, r->venue, r->fighter, profile, name, replay, r->opponents);
    for (i = 0; i < r->opponents && i < ONLINE_OPPONENTS && n < size; i++) {
        char who[NETPLAY_NAME_LEN], id[ONLINE_ID_LEN];
        plain(who, sizeof who, r->opponent[i]);
        online_clean_id(id, r->opponent_id[i]);
        n += (size_t)snprintf(out + n, size - n, "\t%s\t%s", who, id[0] ? id : "-");
    }
}

int online_record_parse(const char *line, OnlineMatchRecord *r)
{
    char copy[1024], *field[8 + 3 + 2 * ONLINE_OPPONENTS], *p;
    int count = 0, i;
    snprintf(copy, sizeof copy, "%s", line);
    copy[strcspn(copy, "\r\n")] = 0;
    for (p = copy; count < (int)(sizeof field / sizeof field[0]); ) {
        field[count++] = p;
        if (!(p = strchr(p, '\t'))) break;
        *p++ = 0;
    }
    if (count < 11 || strcmp(field[0], "1")) return 0;
    memset(r, 0, sizeof *r);
    r->when = strtoll(field[1], NULL, 10);
    r->result = field[2][0];
    if (!strchr("WLDU", r->result) || !r->result) return 0;
    r->counted = atoi(field[3]) != 0 && r->result != 'U';
    r->type = atoi(field[4]);
    r->venue = atoi(field[5]);
    r->fighter = atoi(field[6]);
    unhex_text(r->profile, sizeof r->profile, field[7]);
    snprintf(r->name, sizeof r->name, "%s", field[8]);
    if (strcmp(field[9], "-")) {
        /* a file name, never a path */
        if (strpbrk(field[9], "/\\:") || field[9][0] == '.') return 0;
        snprintf(r->replay, sizeof r->replay, "%s", field[9]);
    }
    r->opponents = atoi(field[10]);
    if (r->opponents < 0) r->opponents = 0;
    if (r->opponents > ONLINE_OPPONENTS) r->opponents = ONLINE_OPPONENTS;
    for (i = 0; i < r->opponents; i++) {
        if (11 + 2 * i + 1 >= count) { r->opponents = i; break; }
        snprintf(r->opponent[i], sizeof r->opponent[i], "%s", field[11 + 2 * i]);
        online_clean_id(r->opponent_id[i], field[12 + 2 * i]);
    }
    if (!r->opponents) r->counted = 0;
    return 1;
}

static int history_path(char *out, size_t size)
{
    char dir[1024];
    if (!online_dir(dir, sizeof dir)) return 0;
    snprintf(out, size, "%s" SEP "history.txt", dir);
    return 1;
}

int online_record_add(const OnlineMatchRecord *r)
{
    char path[1100], line[1024];
    FILE *f;
    int ok;
    if (!history_path(path, sizeof path) || !(f = fopen(path, "a"))) return 0;
    online_record_line(r, line, sizeof line);
    ok = fprintf(f, "%s\n", line) > 0;
    fclose(f);
    return ok;
}

int online_history(OnlineMatchRecord *out, int max)
{
    char path[1100], line[1024];
    FILE *f;
    int count = 0, i;
    if (max <= 0 || !history_path(path, sizeof path) || !(f = fopen(path, "r"))) return 0;
    /* Oldest first in the file; keep the last `max` in a ring, then turn them round. */
    {
        OnlineMatchRecord r;
        long total = 0;
        while (fgets(line, sizeof line, f))
            if (online_record_parse(line, &r)) out[total++ % max] = r;
        count = total < max ? (int)total : max;
        if (total > max) {
            /* the oldest kept is at total % max: rotate so out[0] is the oldest */
            OnlineMatchRecord *copy = (OnlineMatchRecord *)malloc(sizeof *copy * (size_t)max);
            if (copy) {
                for (i = 0; i < max; i++) copy[i] = out[(total + i) % max];
                memcpy(out, copy, sizeof *copy * (size_t)max);
                free(copy);
            }
        }
    }
    fclose(f);
    for (i = 0; i < count / 2; i++) {
        OnlineMatchRecord t = out[i];
        out[i] = out[count - 1 - i];
        out[count - 1 - i] = t;
    }
    return count;
}

/* ---- What the launcher shows ---------------------------------------------------- */
const char *online_title(int wins)
{
    static const struct { int wins; const char *title; } titles[] = {
        {100, "Legend of the city"}, {60, "Kingpin"}, {30, "Enforcer"}, {15, "Brawler"}, {5, "Contender"}, {0, "Rookie"},
    };
    size_t i;
    for (i = 0; i < sizeof titles / sizeof titles[0]; i++)
        if (wins >= titles[i].wins) return titles[i].title;
    return "Rookie";
}

void online_stats(const OnlineMatchRecord *h, int count, OnlineStats *out)
{
    int i, j, run = 0;
    memset(out, 0, sizeof *out);
    out->favourite = out->best_venue = -1;
    /* history is newest first: the current streak from the front, the rest oldest first */
    for (i = 0; i < count; i++) {
        if (!h[i].counted || h[i].result == 'D') { if (h[i].counted) break; continue; }
        if (!out->streak) out->streak = h[i].result == 'W' ? 1 : -1;
        else if ((out->streak > 0) == (h[i].result == 'W')) out->streak += out->streak > 0 ? 1 : -1;
        else break;
    }
    for (i = count - 1; i >= 0; i--) {
        if (!h[i].counted) continue;
        out->matches++;
        if (h[i].result == 'W') out->wins++, run++;
        else {
            if (h[i].result == 'L') out->losses++;
            else out->draws++;
            run = 0;
        }
        if (run > out->best_streak) out->best_streak = run;
    }
    for (i = 0; i < count; i++) {
        int same = 0, wins = 0, losses = 0;
        if (h[i].result == 'U') continue;
        for (j = 0; j < count; j++)
            if (h[j].result != 'U' && h[j].fighter == h[i].fighter && !strcmp(h[j].profile, h[i].profile)) same++;
        if (same > out->favourite_matches) {
            out->favourite_matches = same;
            out->favourite = h[i].fighter;
            snprintf(out->favourite_profile, sizeof out->favourite_profile, "%s", h[i].profile);
        }
        if (!h[i].counted) continue;
        for (j = 0; j < count; j++)
            if (h[j].counted && h[j].venue == h[i].venue) {
                wins += h[j].result == 'W';
                losses += h[j].result == 'L';
            }
        if (wins + losses >= 3 && (wins > out->best_venue_wins
                                   || (wins == out->best_venue_wins && losses < out->best_venue_losses))) {
            out->best_venue = h[i].venue;
            out->best_venue_wins = wins;
            out->best_venue_losses = losses;
        }
    }
}

int online_rivals(const OnlineMatchRecord *h, int count, OnlineRival *out, int max)
{
    int n = 0, i, k, r;
    int *run = (int *)calloc((size_t)(max > 0 ? max : 1), sizeof *run);
    if (!run) return 0;
    for (i = count - 1; i >= 0; i--) {      /* oldest first, so the last name wins */
        for (k = 0; k < h[i].opponents; k++) {
            const char *id = h[i].opponent_id[k];
            if (!id[0]) continue;
            for (r = 0; r < n && strcmp(out[r].id, id); r++) {}
            if (r == n) {
                if (n == max) continue;
                memset(&out[n], 0, sizeof out[n]);
                snprintf(out[n].id, sizeof out[n].id, "%s", id);
                run[n] = 0;
                n++;
            }
            snprintf(out[r].name, sizeof out[r].name, "%s", h[i].opponent[k]);
            if (h[i].when > out[r].last) out[r].last = h[i].when;
            if (!h[i].counted) continue;
            out[r].matches++;
            if (h[i].result == 'W') out[r].wins++, run[r]++;
            else {
                if (h[i].result == 'L') out[r].losses++;
                else out[r].draws++;
                run[r] = 0;
            }
            if (run[r] > out[r].best_streak) out[r].best_streak = run[r];
        }
    }
    free(run);
    /* most matches first, then the most recent */
    for (i = 1; i < n; i++)
        for (k = i; k > 0 && (out[k].matches > out[k - 1].matches
                              || (out[k].matches == out[k - 1].matches && out[k].last > out[k - 1].last)); k--) {
            OnlineRival t = out[k];
            out[k] = out[k - 1];
            out[k - 1] = t;
        }
    return n;
}

/* ---- Replays on disk ------------------------------------------------------------ */
static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void online_prune_replays(const char *dir, int keep)
{
    char **names = NULL, path[1300];
    size_t count = 0, cap = 0, i;
#if defined(_WIN32)
    WIN32_FIND_DATAA found;
    HANDLE h;
    snprintf(path, sizeof path, "%s" SEP "*.djr", dir);
    if ((h = FindFirstFileA(path, &found)) == INVALID_HANDLE_VALUE) return;
    do {
        const char *name = found.cFileName;
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        size_t n = strlen(name);
        if (n < 4 || strcmp(name + n - 4, ".djr")) continue;
#endif
        if (count == cap) {
            char **more = (char **)realloc(names, (cap = cap ? cap * 2 : 64) * sizeof *names);
            if (!more) break;
            names = more;
        }
        if ((names[count] = (char *)malloc(strlen(name) + 1)) != NULL) strcpy(names[count++], name);
#if defined(_WIN32)
    } while (FindNextFileA(h, &found));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    if (count > 1) qsort(names, count, sizeof *names, by_name);
    for (i = 0; i < count; i++) {
        if (keep >= 0 && i + (size_t)keep < count) {
            snprintf(path, sizeof path, "%s" SEP "%s", dir, names[i]);
            remove(path);
        }
        free(names[i]);
    }
    free(names);
}
