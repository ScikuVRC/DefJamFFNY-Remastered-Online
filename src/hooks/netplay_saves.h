/* A created fighter's profile under another name: so that two players whose profiles share a
 * name can both bring theirs to a match (netplay.c, place_saves). */
#ifndef DEFJAM_NETPLAY_SAVES_H
#define DEFJAM_NETPLAY_SAVES_H

#include <stddef.h>

/* Longest name netplay_other_name gives: the profile's name field holds 19 characters and
 * its end; the game's own names are shorter. */
#define NETPLAY_SAVE_NAME_MAX 15

/* The folder (UserData/45410049/<folder>) the game keeps the save `name` in: 12 hex
 * digits from the name (scripts/save-import.py folder_name, src/hooks/save_compat.c). */
void netplay_save_dir_name(const char *name, char out[13]);

/* The profile file `data` renamed `name` in place: the name at the start of its payload and
 * the three signatures over it. 0 when `data` is not a profile file or `name` does not fit. */
int netplay_rename_profile(unsigned char *data, size_t size, const char *name);

/* SaveMeta.xbx for the save `name` (UTF-16LE with a BOM, "Name=<name>\r\n"): its size, or 0
 * when `out` is too small. */
size_t netplay_save_meta(const char *name, unsigned char *out, size_t size);

/* Another name for the profile `name`, the k-th (k >= 2): "MARC2", "MARC3", ... cut so it
 * stays within NETPLAY_SAVE_NAME_MAX. */
void netplay_other_name(const char *name, int k, char *out, size_t size);

#endif
