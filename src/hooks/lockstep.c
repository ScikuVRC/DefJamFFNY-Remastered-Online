/*
 * Lockstep mode (RECOMP_LOCKSTEP=1): what netplay needs from the game itself.
 *
 * Two copies of the game that are given the same fight inputs must play the same
 * fight. With the test probes this was shown on one machine (docs/research/
 * crowd-nondeterminism.md, docs/research/fight-determinism.md); this mode turns the
 * same controls on without the probes:
 *
 *  - every generator a fight draws from is seeded from one number: the match
 *    generator (seed and additive term), the four per-fighter AI generators, and
 *    CLogic's, which the game seeds from the time-stamp counter;
 *  - the crowds are made repeatable (crowd_control.c: hold, spect, reseed);
 *  - first-time help tips are off (defjam_lockstep_tip);
 *  - the fight step's draws from the utility generator come from a private copy
 *    (defjam_lockstep_utility_swap), not the one UI code draws from every frame.
 *
 * The seed is RECOMP_LOCKSTEP_SEED (decimal or 0x hex), the same on both machines;
 * without it a fixed default is used, which is still the same everywhere.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp/gen/recomp_types.h"
#include "guest_call.h"
#include "lockstep.h"
#include "test_observations.h"

#define DEFAULT_SEED       20261006u
#define DEFAULT_INCREMENT  0x382D28A3u  /* what CLogic's constructor gives the first match */

int defjam_lockstep_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_LOCKSTEP");
        enabled = value && !strcmp(value, "1");
        if (enabled)
            fprintf(stderr, "[LOCKSTEP] on: seeds pinned, crowds repeatable\n");
    }
    return enabled;
}

int defjam_pinned_seed(uint32_t *seed)
{
    const char *text;
    if (defjam_lockstep_enabled()) {
        text = getenv("RECOMP_LOCKSTEP_SEED");
        *seed = text && *text ? (uint32_t)strtoul(text, NULL, 0) : DEFAULT_SEED;
        return 1;
    }
    if (!defjam_test_observations_enabled()) return 0;
    text = getenv("RECOMP_TEST_RNG_SEED");
    if (!text || !*text) return 0;
    *seed = (uint32_t)strtoul(text, NULL, 0);
    return 1;
}

/* sub_000D6670(event): the first-time help (CFirstTimeHelp, the global at 0x346A68)
 * is told of an event, such as a spectator holding a weapon, and shows its tip unless
 * its enabled byte (+0x184, 0x346BEC) is 0. A tip pauses the fight until A is
 * pressed, and which tips a player still gets depends on their own save, so the two
 * machines would part there. Lockstep clears the byte before every check; the only
 * other way to the tip (sub_000D5560) is through this function. */
#define HELP_ENABLED 0x00346BECu

void defjam_lockstep_tip(void)
{
    static int reported;
    if (!defjam_lockstep_enabled() || !MEM8(HELP_ENABLED)) return;
    MEM8(HELP_ENABLED) = 0;
    if (!reported++)
        fprintf(stderr, "[LOCKSTEP] first-time help off\n");
}

/* The utility generator (an object-form generator at 0x39C0E0: table, state at +0x80,
 * additive term at +0x84) is drawn once a frame by UI code and inside the fight step
 * by sub_000AC590 (which can pick slow motion). On two machines the frames between
 * steps differ, so the step would draw different values. In lockstep the step's use
 * of it (sub_000AC590, called by the step and by match set-up) gets a private copy,
 * seeded through the game's own routine at every match start. */
#define UTILITY_RNG      0x0039C0E0u
#define UTILITY_RNG_SIZE 0x88u
extern void sub_000AA0D0(void);
static uint32_t s_util_private[UTILITY_RNG_SIZE / 4], s_util_saved[UTILITY_RNG_SIZE / 4];
static int s_util_seeded;

static void utility_seed(void)
{
    uint32_t seed, args[2], i;
    if (!defjam_pinned_seed(&seed)) return;
    /* Seed the real object, keep the result as the private copy, put the real one back. */
    for (i = 0; i < UTILITY_RNG_SIZE / 4; i++) s_util_saved[i] = MEM32(UTILITY_RNG + i * 4);
    args[0] = seed ^ 0x55AA55AAu;
    args[1] = 0x0F0Fu;
    guest_call(sub_000AA0D0, UTILITY_RNG, 2, args);
    for (i = 0; i < UTILITY_RNG_SIZE / 4; i++) {
        s_util_private[i] = MEM32(UTILITY_RNG + i * 4);
        MEM32(UTILITY_RNG + i * 4) = s_util_saved[i];
    }
    s_util_seeded = 1;
}

/* sub_001A4D90: a match is being set up. */
void defjam_lockstep_match_start(void)
{
    if (defjam_lockstep_enabled()) utility_seed();
}

/* The utility generator's state word as the fight step sees it: the private copy's in
 * lockstep, the real one's otherwise (for the state stream and the fingerprint). */
uint32_t defjam_lockstep_utility_state(void)
{
    if (defjam_lockstep_enabled() && s_util_seeded)
        return s_util_private[0x80 / 4];
    return MEM32(UTILITY_RNG + 0x80);
}

int defjam_lockstep_utility_swap(int enter)
{
    uint32_t i;
    if (!defjam_lockstep_enabled()) return 0;
    if (enter && !s_util_seeded) utility_seed();
    if (!s_util_seeded) return 0;
    for (i = 0; i < UTILITY_RNG_SIZE / 4; i++) {
        uint32_t addr = UTILITY_RNG + i * 4;
        if (enter) {
            s_util_saved[i] = MEM32(addr);
            MEM32(addr) = s_util_private[i];
        } else {
            s_util_private[i] = MEM32(addr);
            MEM32(addr) = s_util_saved[i];
        }
    }
    return 1;
}

int defjam_pinned_increment(uint32_t *increment)
{
    if (!defjam_lockstep_enabled()) return 0;
    *increment = DEFAULT_INCREMENT;
    return 1;
}

/* The global add-with-carry generator at 0x33DB5C (six words, sub_0002A390) has 236
 * draw sites, the menus among them, so its state when a match is set up follows the
 * frames spent in the menus. A match's set-up draws from it too: a created fighter's
 * intro, for one, came out different on two games that had spent different times on
 * the fighter screen, and the fight parted some 700 steps later (2026-10-30). In
 * lockstep it is seeded from the lockstep seed when the match is set up, the same way
 * the spectators' private copy is (crowd_control.c). */
#define SHARED_RNG       0x0033DB5Cu
#define SHARED_RNG_WORDS 6

void defjam_lockstep_shared_seed(void)
{
    uint32_t seed;
    int i;
    if (!defjam_lockstep_enabled() || !defjam_pinned_seed(&seed)) return;
    seed ^= SHARED_RNG;
    for (i = 0; i < SHARED_RNG_WORDS - 1; i++) {
        seed = seed * 1664525u + 1013904223u;
        MEM32(SHARED_RNG + (uint32_t)i * 4) = seed;
    }
    MEM32(SHARED_RNG + (SHARED_RNG_WORDS - 1) * 4) = 0;    /* +0x14 counts draws */
}

/* The main loop runs 0 to 4 steps per presented frame, one per queued 60 Hz record
 * (docs/research/simulation-tick.md): the gate sub_000D75E0 on the frame object (its
 * address at 0x3A017C) counts its calls in a frame at +0x1A5C and stops at the fifth.
 * What is drawn is updated once a frame, and a created fighter's skeleton is read back
 * from what was last drawn, so how steps and frames interleaved -- which follows the
 * host's timing -- reached the fight: two games with the same inputs parted after a few
 * hundred steps (2026-10-30). In lockstep every pass through the step marks the frame's
 * count as used up, so a frame runs at most one step and every step follows a frame. A
 * second record queued in the same frame is then dropped instead of run, which costs
 * speed when the host stutters but never changes the fight. (The gate's own one-a-frame
 * mode, +0x1A54, runs every other frame: half speed.) */
#define FRAME_OBJECT_SLOT   0x003A017Cu
#define FRAME_CALLS         0x1A5Cu
#define FRAME_CALLS_LAST    4

void defjam_lockstep_tick(void)
{
    static int said;
    uint32_t frames;
    if (!defjam_lockstep_enabled()) return;
    frames = MEM32(FRAME_OBJECT_SLOT);
    if (!frames) return;
    MEM32(frames + FRAME_CALLS) = FRAME_CALLS_LAST;
    if (!said) {
        said = 1;
        fprintf(stderr, "[LOCKSTEP] one fight step per presented frame\n");
    }
}
