/*
 * File mods: a pack's files in place of the game's (docs/mod-manager.md).
 *
 * A pack in the data folder's mods directory may have a files folder laid out like
 * the game dir:
 *
 *     <data>/mods/<pack>/files/music/dirty/main.mus
 *     <data>/mods/<pack>/files/assets/v2bg.viv
 *
 * Whenever the game opens a file of its own (music, speech, models in the .viv
 * archives, arenas, the front end's movies), the packs named in [mods] packs (or
 * RECOMP_FILE_PACKS), semicolon-separated, are asked first; the last pack that has
 * the file wins, as with texture packs. Nothing is copied or changed in the game dir.
 * A replacement has to be in the format the game reads: the same container and codec
 * as the file it replaces. A file no pack has goes on to the redirect that was there
 * before (a language pack's, pc_language.c), so a file pack wins over a language pack.
 *
 * Every pack in a netplay match must be the same on every game; a pack that changes
 * the fight (models, game data) makes the games part.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "recomp_settings.h"
#include "kernel/kernel.h"

#define MAX_PACKS   16

static char s_root[MAX_PACKS][1024];        /* <data>/mods/<pack>/files, last listed first */
static int s_count;
static BOOL (*s_next)(const char *relative, xbox_host_char *host_path, DWORD size);

static int exists(const char *path, xbox_host_char *out, DWORD size)
{
#if defined(_WIN32)
    DWORD attrs;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, out, (int)size)) return 0;
    attrs = GetFileAttributesW(out);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (snprintf(out, size, "%s", path) >= (int)size) return 0;
    return stat(out, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static BOOL replace(const char *relative, xbox_host_char *host_path, DWORD size)
{
    char path[2048], *p;
    int i;
    if (!relative || !*relative || strstr(relative, ".."))
        return s_next ? s_next(relative, host_path, size) : FALSE;
    for (i = 0; i < s_count; i++) {
        snprintf(path, sizeof path, "%s/%s", s_root[i], relative);
        for (p = path; *p; p++)
#if defined(_WIN32)
            if (*p == '/') *p = '\\';
#else
            if (*p == '\\') *p = '/';
#endif
        if (exists(path, host_path, size)) {
            fprintf(stderr, "[MODS] %s from %s\n", relative, s_root[i]);
            return TRUE;
        }
    }
    return s_next ? s_next(relative, host_path, size) : FALSE;
}

void defjam_file_mods_init(void)
{
    char packs[RECOMP_SETTING_TEXT_MAX], *name, *next;
    const char *data = getenv("DEFJAM_DATA"), *env = getenv("RECOMP_FILE_PACKS");
    char found[MAX_PACKS][1024];
    int n = 0, i;
    if (env) snprintf(packs, sizeof packs, "%s", env);
    else recomp_settings_get_text("mods", "packs", packs, sizeof packs, "");
    for (name = packs; name && *name && n < MAX_PACKS; name = next) {
        next = strchr(name, ';');
        if (next) *next++ = 0;
        while (*name == ' ') name++;
        if (!*name || strpbrk(name, "/\\:") || strstr(name, "..")) continue;
#if defined(_WIN32)
        snprintf(found[n++], sizeof found[0], "%s\\mods\\%s\\files", data && *data ? data : ".", name);
#else
        snprintf(found[n++], sizeof found[0], "%s/mods/%s/files", data && *data ? data : ".", name);
#endif
    }
    /* The last pack listed is asked first. */
    for (i = 0; i < n; i++) snprintf(s_root[i], sizeof s_root[i], "%s", found[n - 1 - i]);
    s_count = n;
    if (n) {
        s_next = g_xbox_game_file_redirect;     /* set up first: pc_settings.c starts this after the language */
        g_xbox_game_file_redirect = replace;
        fprintf(stderr, "[MODS] %d file pack(s): %s\n", n, packs);
    }
}
