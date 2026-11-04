/*
 * The launcher's quick starts: the game carried past its title screen to the menu the
 * player chose on the launcher's Home, the way an online match is (netplay.c).
 *
 * The intro is skipped (skip_intro.c asks launcher_start_target) and a pad script presses
 * for the player: START and A until the main menu; the menu's own choice once its music
 * starts (it takes no input before, and with HD texture packs its art loads first, as the
 * test routes found, scripts/harness.py); then, for Story, on the Select User ID list,
 * DOWN to the profile and A. That list starts on CREATE USER ID with the profiles below it
 * in their folders' order (netplay_profiles), and takes no input until it has read them:
 * it asks MemCard.GetNumUsers() first, so the presses count from there (2026-11-03: with
 * eleven profiles, presses 5 s after the screen came up were lost). Battle goes straight
 * to Choose Match Type, and its players pick their user IDs after that on the game's own
 * Battle ID screen, which asks GetNumUsers too: Story's presses would land there. The
 * player's own pad stays live (RECOMP_PAD_HOST) and takes over where the script ends.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "launcher_start.h"

/* A held direction repeats after about a quarter of a second. Windows sees a press later,
 * and its test routes all hold 0.3 s; macOS moved twice with that (harness.py). */
#if defined(_WIN32)
#define HOLD        0.3
#else
#define HOLD        0.2
#endif
#define FIRST_DOWN  1.5         /* after MemCard.GetNumUsers(): the list is built from it */
#define DOWN_STEP   0.6

static int s_target = LAUNCHER_START_TITLE;

static void put_env(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void add(char *out, size_t size, size_t *n, const char *format, ...)
{
    va_list args;
    int k;
    if (*n >= size) return;
    va_start(args, format);
    k = vsnprintf(out + *n, size - *n, format, args);
    va_end(args);
    *n = k < 0 ? size : *n + (size_t)k;
}

/* START every other second and A in between: START takes the title screen once it is up
 * (about 7 s in), A the memory-card notice. The main menu ends them. */
size_t launcher_title_presses(char *out, size_t size)
{
    size_t n = 0;
    int i;
    if (size) out[0] = 0;
    for (i = 0; i < 40; i++)
        add(out, size, &n, "%s%s:%d:%d.3", i ? "," : "", i % 2 ? "a" : "start", 1 + i, 1 + i);
    add(out, size, &n, ",@getscreeninfo(intmain/mainmenu");
    return n < size ? n : 0;
}

size_t launcher_start_script(int target, int user, char *out, size_t size)
{
    size_t n;
    double t;
    int i;
    if (size) out[0] = 0;
    if (target <= LAUNCHER_START_TITLE || target > LAUNCHER_START_BATTLE)
        return 0;
    if (!(n = launcher_title_presses(out, size)))
        return 0;
    if (target == LAUNCHER_START_STORY)             /* the menu's first item */
        add(out, size, &n, ",@audio.playmusic(,a:3:3.3");
    else if (target == LAUNCHER_START_BATTLE)       /* one to the right */
        add(out, size, &n, ",@audio.playmusic(,right:3:%g,a:6:6.3", 3 + HOLD);
    if (target == LAUNCHER_START_STORY && user >= 0) {
        add(out, size, &n, ",@memcard.getnumusers(");
        for (i = 0, t = FIRST_DOWN; i <= user; i++, t += DOWN_STEP)
            add(out, size, &n, ",down:%g:%g", t, t + HOLD);
        add(out, size, &n, ",a:%g:%g", t + .4, t + .7);
    }
    return n < size ? n : 0;
}

void launcher_start(int target, int user)
{
    static const char *names[] = {"the title screen", "the main menu", "Story", "Battle"};
    char script[1400];
    if (target <= LAUNCHER_START_TITLE || target > LAUNCHER_START_BATTLE)
        return;
    s_target = target;
    /* A test's own pad script is left as it is. */
    if (!getenv("RECOMP_PAD_SCRIPT") && launcher_start_script(target, user, script, sizeof script)) {
        put_env("RECOMP_SCRIPT_ANCHOR", "getscreeninfo(intmain/prestart");
        put_env("RECOMP_PAD_SCRIPT", script);
        put_env("RECOMP_PAD_SCRIPT_FORMAT", "seconds");     /* name:from:to, in seconds */
        put_env("RECOMP_PAD_HOST", "1");
    }
    if (target == LAUNCHER_START_STORY && user >= 0)
        fprintf(stderr, "[LAUNCHER] starting at %s with user ID %d\n", names[target], user + 1);
    else
        fprintf(stderr, "[LAUNCHER] starting at %s\n", names[target]);
    fflush(stderr);
}

int launcher_start_target(void)
{
    return s_target;
}
