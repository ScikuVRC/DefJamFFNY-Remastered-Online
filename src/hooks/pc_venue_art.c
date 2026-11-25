/* Venue pictures for the launcher's venue vote (pc_launcher_select.cpp), taken from the player's
 * own game: the first time a fight has run for a while in a venue that has no picture yet, the
 * frame on screen is saved as <data>/launcher/venues/<venue>.jpg. The game's archives hold no
 * picture of the arenas, so the picture comes from playing there.
 *
 * Cut to 16:9 below the health bars, and lifted a little, since the arenas are dark.
 * RECOMP_NO_VENUE_ART=1 turns it off. */
#include "pc_venue_art.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#define FIGHT_STEPS 600     /* about ten seconds of fighting: the fighters are in the arena, not the intro */

static volatile int s_venue = -1;
static int s_steps, s_done;
static char s_path[1024];
static volatile int s_want;

static int enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_NO_VENUE_ART");
        on = !(e && *e && strcmp(e, "0"));
    }
    return on;
}

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* <data>/launcher/venues, made when missing; the path of the picture for `venue`. */
static int picture_path(int venue, char *out, size_t size)
{
    const char *data = getenv("DEFJAM_DATA");
    char dir[1024];
    if (!data || !*data)
        data = ".";
    snprintf(dir, sizeof dir, "%s/launcher", data);
    make_dir(dir);
    snprintf(dir, sizeof dir, "%s/launcher/venues", data);
    make_dir(dir);
    snprintf(out, size, "%s/%d.png", dir, venue);
    if (exists(out))
        return 0;
    snprintf(out, size, "%s/%d.jpg", dir, venue);
    return !exists(out);
}

void pc_venue_art_match(int venue)
{
    s_venue = venue;
    s_steps = 0;
    s_done = 0;
}

void pc_venue_art_step(int fighting)
{
    if (s_done || s_venue < 0 || !enabled())
        return;
    if (!fighting || ++s_steps < FIGHT_STEPS)
        return;
    s_done = 1;
    if (picture_path(s_venue, s_path, sizeof s_path))
        s_want = 1;
}

const char *pc_venue_art_wanted(void)
{
    return s_want ? s_path : NULL;
}

static void write_bytes(void *context, void *data, int size)
{
    fwrite(data, 1, (size_t)size, (FILE *)context);
}

void pc_venue_art_save(const uint8_t *rgb, int width, int height)
{
    int ch = width * 9 / 16, top, x, y;
    uint8_t *out;
    FILE *f;
    s_want = 0;
    if (width <= 0 || height <= 0 || ch > height)
        return;
    top = height * 23 / 100;            /* below the health bars */
    if (top > height - ch)
        top = height - ch;
    out = (uint8_t *)malloc((size_t)width * ch * 3);
    if (!out)
        return;
    for (y = 0; y < ch; y++)
        for (x = 0; x < width * 3; x++) {
            /* Brighter by a third, with a little more contrast. */
            int v = rgb[((size_t)(top + y) * width) * 3 + x];
            v = (int)((v * 1.35f - 128.0f) * 1.1f + 128.0f);
            out[((size_t)y * width) * 3 + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    f = fopen(s_path, "wb");
    if (f) {
        stbi_write_jpg_to_func(write_bytes, f, width, ch, 3, out, 90);
        fclose(f);
        fprintf(stderr, "[VENUE-ART] saved %s\n", s_path);
    }
    free(out);
}
