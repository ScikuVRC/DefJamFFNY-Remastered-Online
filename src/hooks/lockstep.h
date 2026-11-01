#ifndef DEFJAM_LOCKSTEP_H
#define DEFJAM_LOCKSTEP_H
/* Lockstep mode: the game state a fight is made of becomes a function of the fight's
 * inputs and one seed, so two machines given the same inputs play the same fight
 * (the ground netplay stands on; src/hooks/lockstep.c). */
#include <stdint.h>

/* RECOMP_LOCKSTEP=1. Off by default; a normal game is never changed by this. */
int defjam_lockstep_enabled(void);

/* The seed every pinned generator is derived from, when one is pinned: the lockstep
 * seed (RECOMP_LOCKSTEP_SEED, or a fixed default) in lockstep mode, else the test
 * seed (RECOMP_TEST_RNG_SEED with RECOMP_TEST_OBSERVATIONS=1). Returns 0 when nothing
 * is pinned and the game keeps its own seeds. */
int defjam_pinned_seed(uint32_t *seed);

/* The match generator's additive term, pinned only in lockstep mode: the game takes it
 * from the time-stamp counter for every match after a process's first. */
int defjam_pinned_increment(uint32_t *increment);

/* The utility generator's state word (0x39C160) as the fight step sees it: in lockstep
 * the step draws from a private copy, which the menus' draws do not move. */
uint32_t defjam_lockstep_utility_state(void);

/* At match set-up (with the AI seeds): the shared generator at 0x33DB5C, which the menus
 * draw from every frame, seeded from the lockstep seed. */
void defjam_lockstep_shared_seed(void);

/* Every pass of the main loop's step (the run-module manager's tick): in lockstep, one
 * fight step per presented frame (see lockstep.c). */
void defjam_lockstep_tick(void);
#endif
