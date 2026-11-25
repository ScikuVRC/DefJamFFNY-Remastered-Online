/* Venue pictures for the launcher, saved from the player's own fights (pc_venue_art.c). */
#ifndef DEFJAM_PC_VENUE_ART_H
#define DEFJAM_PC_VENUE_ART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A match is starting in `venue` (the id Game.ChooseVenue carries). */
void pc_venue_art_match(int venue);
/* Each match step; `fighting` while the fight is under way. */
void pc_venue_art_step(int fighting);
/* Where the picture goes when the next presented frame should be saved, else NULL. */
const char *pc_venue_art_wanted(void);
/* Save that frame: `rgb` is width x height, top row first. */
void pc_venue_art_save(const uint8_t *rgb, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
