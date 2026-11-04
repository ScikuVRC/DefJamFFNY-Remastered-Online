/* Where the launcher's tiles start the game (pc_launcher_home.cpp). */

#ifndef DEFJAM_LAUNCHER_START_H
#define DEFJAM_LAUNCHER_START_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LAUNCHER_START_TITLE = 0,   /* as before: the intro, then Press Start */
    LAUNCHER_START_MENU,        /* the main menu */
    LAUNCHER_START_STORY,       /* Story with a profile: on to its crib */
    LAUNCHER_START_BATTLE       /* Battle: on to Choose Match Type (each player picks a
                                   user ID after it, on the game's own screen) */
};

/* START and A from the title screen until the main menu, in RECOMP_PAD_SCRIPT's seconds
 * format on the clock of RECOMP_SCRIPT_ANCHOR=getscreeninfo(intmain/prestart. Shared with
 * an online match's start (netplay.c). Returns the length written. */
size_t launcher_title_presses(char *out, size_t size);

/* The whole script for `target`; `user` is Story's profile, its place in netplay_profiles'
 * order, -1 for none. Returns the length written, 0 for LAUNCHER_START_TITLE. */
size_t launcher_start_script(int target, int user, char *out, size_t size);

/* Set the environment for it, before the game starts. */
void launcher_start(int target, int user);

/* What launcher_start was asked for; LAUNCHER_START_TITLE when it was not called. */
int launcher_start_target(void);

#ifdef __cplusplus
}
#endif

#endif
