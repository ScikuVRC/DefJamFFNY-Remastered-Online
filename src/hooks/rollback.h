#ifndef DEFJAM_ROLLBACK_H
#define DEFJAM_ROLLBACK_H
/* Rollback (src/hooks/rollback.c, docs/netplay.md): the whole state a fight step reads and
 * writes, saved and put back. The guest's memory (one block: RAM and heap; the high
 * addresses are mirrors of it) and the host-side state of the hooks that take part in a
 * lockstep fight, which each module lists itself (rollback_add). */
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RollbackState RollbackState;

/* A saved state; reused, so a save costs no allocation after the first. */
RollbackState *rollback_new(void);
void rollback_free(RollbackState *s);
/* Save / put back the game's state. 0 when memory could not be had. */
int rollback_save(RollbackState *s);
void rollback_restore(const RollbackState *s);

/* Leave guest range [first, end) as it is on every restore (another thread's, not the fight's). */
void rollback_skip(uint32_t first, uint32_t end);
/* The journal: a cheap save at the start of every tick (`tick` counts up), and back to one of
 * the last 64 (0 when it doesn't reach). */
void rollback_journal_mark(long tick);
int rollback_journal_restore(long tick, long last_tick);
/* A stream library pool was set up (recomp_manual.c, sub_001E7900): rewinds leave it alone. */
void rollback_stream_pool(uint32_t at, uint32_t size);
/* How the guest memory now differs from the saved: changed 4 KiB pages, and up to `max`
 * ranges of them as guest addresses (first, last + 1). */
int rollback_compare(const RollbackState *s, uint32_t (*ranges)[2], int max, int *range_count);
/* Each changed page's changed byte spans, to `out` (diagnostics). */
void rollback_dump_changes(const RollbackState *s, FILE *out);
/* The guest memory's span: its first guest address and its size. */
void rollback_memory(uint32_t *guest_base, size_t *size);

/* Track what a fight step writes: begin before the step, end after it (on the step's thread).
 * The bytes the step itself changes are kept as the fight's; rollback_restore_fight puts back
 * those and the hooks' host state, nothing else. Windows only for now. */
void rollback_watch(uint32_t guest);    /* log every write to a guest address (diagnostics) */
void rollback_learn_step(long step);    /* RECOMP_ROLLBACK_LEARN: which thread writes which page */
void rollback_own_clear(uint32_t addr, uint32_t size); /* the fight's thread was given this block */
void rollback_own_reset(void);                  /* a fight starts */
int rollback_crt_list_check(const char *when, long step);
long rollback_own_count(void);                  /* pages with bytes that aren't the fight's */
uint32_t rollback_sound_alloc(uint32_t size);   /* 0: not on, or full */
int rollback_sound_free(uint32_t addr);         /* 1: it was the sound engine area's */
int rollback_defer_free(uint32_t addr);          /* another thread's free of a game heap block */
uint32_t rollback_deferred_pop(void);            /* the next such free, for the fight's thread */
int rollback_heapfix(void);
void rollback_lib_enter(void);
void rollback_lib_leave(void);
int rollback_in_library(void);
void rollback_sound_begin(void);                /* a sound call starts on the fight's thread */
int rollback_sound_end(uint32_t (*out)[2], int *count);
void rollback_skip_audio(void);                 /* the sound system's fixed blocks */
void rollback_track_begin(void);
void rollback_track_end(void);
void rollback_track_stats(long *steps, long *fight_bytes, long *other_pages);
void rollback_restore_fight(const RollbackState *s);
void rollback_dump_mask(FILE *out, size_t gap);

/* Host-side state that is part of a fight: each module that keeps some lists it here (called
 * once per save and restore, with `add` taking each block). */
typedef void (*rollback_block_fn)(void *data, size_t size);
void defjam_crowd_rollback_blocks(rollback_block_fn add);
void defjam_lockstep_rollback_blocks(rollback_block_fn add);

#ifdef __cplusplus
}
#endif
#endif
