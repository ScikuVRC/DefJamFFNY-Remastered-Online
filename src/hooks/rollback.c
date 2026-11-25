/* Rollback's saved states (rollback.h). A state is the guest memory block, copied whole, and
 * the hooks' host-side blocks (crowd_control.c, lockstep.c) appended after it. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rollback.h"
#include "recomp/gen/recomp_types.h"
#include "kernel/kernel.h"

extern void *xbox_GetMemoryBase(void);
extern size_t xbox_GetMappedSize(void);

#define GUEST_BASE  0x00010000u     /* XBOX_BASE_ADDRESS */
#define PAGE        4096u
/* The contiguous window: separate storage from RAM here (xbox_memory_layout.c), and where the
 * game's heap of animation sets, the mode manager and much else live. */
#define CONTIG_BASE 0x80000000u
#define CONTIG_SIZE XBOX_CONTIG_SIZE

struct RollbackState {
    unsigned char *memory;
    size_t size;
    unsigned char *contig;
    unsigned char *host;            /* the hooks' blocks, one after another */
    size_t host_size, host_used;
    int ok;
    /* The guest CPU's registers on the saving thread: the same code position is resumed, so
     * they are live state like the memory is. */
    uint32_t regs[9];
    double fp[8];
    int fp_top, fp_cmp;
    uint16_t fp_cc, fp_cw;
    uint32_t mxcsr;
    RecompXmm xmm[8];
};

void rollback_memory(uint32_t *guest_base, size_t *size)
{
    *guest_base = GUEST_BASE;
    *size = xbox_GetMappedSize();
}

RollbackState *rollback_new(void)
{
    RollbackState *s = (RollbackState *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->size = xbox_GetMappedSize();
    s->memory = (unsigned char *)malloc(s->size ? s->size : 1);
    s->contig = (unsigned char *)malloc(CONTIG_SIZE);
    if (!s->memory || !s->contig) { free(s->memory); free(s->contig); free(s); return NULL; }
    return s;
}

void rollback_free(RollbackState *s)
{
    if (!s) return;
    free(s->memory);
    free(s->contig);
    free(s->host);
    free(s);
}

static void regs_save(RollbackState *s)
{
    s->regs[0] = g_eax; s->regs[1] = g_ecx; s->regs[2] = g_edx; s->regs[3] = g_ebx; s->regs[4] = g_esi;
    s->regs[5] = g_edi; s->regs[6] = g_ebp; s->regs[7] = g_seh_ebp; s->regs[8] = g_esp;
    memcpy(s->fp, g_fp_stack, sizeof s->fp);
    s->fp_top = g_fp_top; s->fp_cmp = g_fp_cmp; s->fp_cc = g_fp_cc; s->fp_cw = g_fp_control_word; s->mxcsr = g_mxcsr;
    s->xmm[0] = g_xmm0; s->xmm[1] = g_xmm1; s->xmm[2] = g_xmm2; s->xmm[3] = g_xmm3;
    s->xmm[4] = g_xmm4; s->xmm[5] = g_xmm5; s->xmm[6] = g_xmm6; s->xmm[7] = g_xmm7;
}

static void regs_restore(const RollbackState *s)
{
    g_eax = s->regs[0]; g_ecx = s->regs[1]; g_edx = s->regs[2]; g_ebx = s->regs[3]; g_esi = s->regs[4];
    g_edi = s->regs[5]; g_ebp = s->regs[6]; g_seh_ebp = s->regs[7];
    if (g_esp != s->regs[8]) fprintf(stderr, "[ROLLBACK] esp differs at the restore: %08x, saved %08x\n", g_esp, s->regs[8]);
    memcpy(g_fp_stack, s->fp, sizeof s->fp);
    g_fp_top = s->fp_top; g_fp_cmp = s->fp_cmp; g_fp_cc = s->fp_cc; g_fp_control_word = s->fp_cw; g_mxcsr = s->mxcsr;
    g_xmm0 = s->xmm[0]; g_xmm1 = s->xmm[1]; g_xmm2 = s->xmm[2]; g_xmm3 = s->xmm[3];
    g_xmm4 = s->xmm[4]; g_xmm5 = s->xmm[5]; g_xmm6 = s->xmm[6]; g_xmm7 = s->xmm[7];
}

/* Which registers differ at a restore (diagnostics). */
static void regs_report(const RollbackState *s)
{
    static const char *const names[8] = {"eax", "ecx", "edx", "ebx", "esi", "edi", "ebp", "seh_ebp"};
    uint32_t now[8];
    int i;
    now[0] = g_eax; now[1] = g_ecx; now[2] = g_edx; now[3] = g_ebx; now[4] = g_esi; now[5] = g_edi; now[6] = g_ebp; now[7] = g_seh_ebp;
    for (i = 0; i < 8; i++)
        if (now[i] != s->regs[i]) fprintf(stderr, "[ROLLBACK] register %s: %08x now, %08x saved\n", names[i], now[i], s->regs[i]);
    if (g_fp_top != s->fp_top) fprintf(stderr, "[ROLLBACK] fp top: %d now, %d saved\n", g_fp_top, s->fp_top);
}

/* The blocks are walked with one cursor: on save they are copied in, on restore out. */
static RollbackState *s_walk;
static int s_walk_restore;

static void walk_block(void *data, size_t size)
{
    RollbackState *s = s_walk;
    if (!s_walk_restore) {
        if (s->host_used + size > s->host_size) {
            size_t want = (s->host_used + size) * 2;
            unsigned char *more = (unsigned char *)realloc(s->host, want);
            if (!more) { s->ok = 0; return; }
            s->host = more;
            s->host_size = want;
        }
        memcpy(s->host + s->host_used, data, size);
    } else if (s->host_used + size <= s->host_size) {
        memcpy(data, s->host + s->host_used, size);
    }
    s->host_used += size;
}

static void walk(RollbackState *s, int restore)
{
    s_walk = s;
    s_walk_restore = restore;
    s->host_used = 0;
    defjam_crowd_rollback_blocks(walk_block);
    defjam_lockstep_rollback_blocks(walk_block);
    s_walk = NULL;
}

int rollback_save(RollbackState *s)
{
    if (!s || !s->memory) return 0;
    s->ok = 1;
    memcpy(s->memory, (const void *)XBOX_PTR(GUEST_BASE), s->size);
    memcpy(s->contig, (const void *)XBOX_PTR(CONTIG_BASE), CONTIG_SIZE);
    regs_save(s);
    walk(s, 0);
    return s->ok;
}

/* ---- Whose bytes are whose, as the game runs (rollback_note_write) ---------------------------
 * With rollback on, every store in the lifted code reports here (scratchpad instrument.py). A
 * store by any thread but the fight's, or by audio code on any thread, marks its bytes as not the
 * fight's, and a restore leaves them alone. Nothing here depends on where a stage, a fighter or
 * the heap happens to put things. Stores made by the host for the game (a file read, the sound
 * chip's model) don't come through here: the fixed list covers those. */
static int s_arena_state_get(void);
static int rollback_in_arena(uint32_t addr);
#define OWN_RAM     0x04000000u
#define OWN_BYTES   (OWN_RAM + CONTIG_SIZE)
#define MAIN_STACK_LOW  0x00F40000u    /* the fight's thread: its stack is how it is told apart */
#define MAIN_STACK_END  0x00F80000u
static unsigned char *s_own;           /* a byte per guest byte: 1 not the fight's */
static unsigned char *s_own_page;      /* a byte per page: 1 some byte in it is marked */
static int s_own_state;                /* 0 not decided, 1 on, -1 off */
static int s_audio_watch, s_audio_logged;
/* Diagnostics: who last wrote each word of the audio heap. */
static uint32_t (*s_writer)[3];
static uint32_t s_writer_heap, s_writer_size;

static void writer_note(uint32_t addr, uint32_t size, uint32_t sp, int fights, int audio)
{
    uint32_t i, k, found[2] = {0, 0}, n = 0;
    for (k = 0; k < 32 && n < 2; k++) {
        uint32_t v = MEM32(sp + k * 4);
        if (v >= 0x00011000u && v < 0x00230000u) found[n++] = v;
    }
    for (i = addr / 4; i <= (addr + size - 1) / 4; i++) {
        uint32_t w = i * 4 - s_writer_heap;
        if (w / 4 < s_writer_size / 4) {
            s_writer[w / 4][0] = found[0];
            s_writer[w / 4][1] = found[1];
            s_writer[w / 4][2] = (fights ? 1u : 0u) | (audio ? 2u : 0u);
        }
    }
}

static void writer_dump_around(uint32_t reg)
{
    uint32_t a;
    if (!s_writer || reg < s_writer_heap || reg >= s_writer_heap + s_writer_size) return;
    for (a = (reg & ~3u) - 0x20; a < (reg & ~3u) + 0x60; a += 4) {
        uint32_t w = (a - s_writer_heap) / 4;
        if (a < s_writer_heap || a >= s_writer_heap + s_writer_size) continue;
        fprintf(stderr, "[ROLLBACK]   %08x = %08x, last written by %06x < %06x, %s, %s\n", a, MEM32(a), s_writer[w][0], s_writer[w][1],
                s_writer[w][2] & 1 ? "fight thread" : "other thread", s_writer[w][2] & 2 ? "audio code" : "plain code");
    }
}

static LONG CALLBACK writer_crash(EXCEPTION_POINTERS *e)
{
    static volatile LONG once;
    uintptr_t base = (uintptr_t)XBOX_PTR(0);
    uintptr_t at;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    at = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    if (at < base || at - base >= 0xF0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "[ROLLBACK] crash reading guest %08x; registers eax %08x ecx %08x edx %08x ebx %08x esi %08x edi %08x\n",
            (uint32_t)(at - base), g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi);
    writer_dump_around(g_esi);
    writer_dump_around(g_ebx);
    writer_dump_around(g_edi);
    writer_dump_around(g_ecx);
    writer_dump_around(g_edx);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void own_init(void)
{
    s_own_state = -1;
    if (!getenv("RECOMP_ROLLBACK_TEST")) return;
    s_own = (unsigned char *)calloc(OWN_BYTES + 0x400000u, 1);
    s_own_page = (unsigned char *)calloc(OWN_BYTES / PAGE + 2048, 1);
    if (s_own && s_own_page) s_own_state = 1;
}

/* A sound call in progress on the fight's thread (rollback_sound_begin), and the fight's stack
 * words it has written above where it started: its output arguments. A re-run skips the call, so
 * it must leave the same values there. */
#define CALL_WRITES 32
static int skip_covers(uint32_t addr);
static void skip_sort(void);
static int s_call_depth;
static uint32_t s_call_sp, s_call_addr[CALL_WRITES];
static int s_call_writes;

void rollback_sound_begin(void)
{
    uint32_t sp = g_esp;
    if (sp < MAIN_STACK_LOW || sp >= MAIN_STACK_END) return;
    if (!s_call_depth++) {
        s_call_sp = sp;
        s_call_writes = 0;
    }
}

/* The call returned. Non-zero for the outermost: out[i] is {address, value}. */
int rollback_sound_end(uint32_t (*out)[2], int *count)
{
    int i, n = 0;
    if (s_call_depth <= 0) return 0;
    if (--s_call_depth) return 0;
    for (i = 0; i < s_call_writes; i++) {
        out[n][0] = s_call_addr[i];
        out[n][1] = MEM32(s_call_addr[i]);
        n++;
    }
    *count = n;
    return 1;
}

/* RECOMP_ROLLBACK_HEAPFIX=1: the game heap is the fight's. Library code allocates elsewhere
 * (recomp_manual.c asks rollback_in_library), and what library code writes into a block of the
 * game heap is a fight object's field (a handle, an output): it is the fight's, rewound with it,
 * and written back by a re-run. Marked "not the fight's", such bytes would outlive the block and
 * keep a rewind from putting back the free list the heap keeps in freed blocks. */
static int s_heapfix = -1;
static int s_lib_depth;

int rollback_heapfix(void)
{
    if (s_heapfix < 0) s_heapfix = getenv("RECOMP_ROLLBACK_HEAPFIX") != NULL;
    return s_heapfix;
}

/* The sound tick, which the job pump runs on the fight's thread (recomp_manual.c, sub_00127550). */
void rollback_lib_enter(void) { if (g_esp >= MAIN_STACK_LOW && g_esp < MAIN_STACK_END) s_lib_depth++; }
void rollback_lib_leave(void) { if (g_esp >= MAIN_STACK_LOW && g_esp < MAIN_STACK_END && s_lib_depth > 0) s_lib_depth--; }

/* On the fight's thread, inside the sound library: a sound call, or the sound tick. */
int rollback_in_library(void)
{
    return g_esp >= MAIN_STACK_LOW && g_esp < MAIN_STACK_END && (s_call_depth > 0 || s_lib_depth > 0);
}

#define GAME_HEAP_FIRST 0x81000000u
#define GAME_HEAP_END   0x83A09000u

void rollback_note_write(unsigned addr, unsigned size)
{
    int audio = (size & 0x40000000u) != 0;
    uint32_t sp = g_esp;
    size_t at;
    unsigned i;
    int fights = sp >= MAIN_STACK_LOW && sp < MAIN_STACK_END;
    int fight_heap = s_heapfix > 0 && addr >= GAME_HEAP_FIRST && addr < GAME_HEAP_END && !skip_covers(addr);
    if (fights && audio && fight_heap) audio = 0;
    if (fights && s_call_depth && ((addr >= s_call_sp && addr < MAIN_STACK_END) || fight_heap)) {
        uint32_t word = addr & ~3u;
        int k;
        for (k = 0; k < s_call_writes && s_call_addr[k] != word; k++) {}
        if (k == s_call_writes && k < CALL_WRITES) s_call_addr[s_call_writes++] = word;
    }
    if (!s_own_state) own_init();
    /* Diagnostics: the audio heap (0x331EC4, 0x331EDC) written by anything but the sound engine. */
    if (s_own_state > 0 && s_audio_watch) {
        uint32_t heap = MEM32(0x331EC4u);
        if (!s_writer && heap) {
            s_writer_heap = heap;
            s_writer_size = MEM32(0x331EDCu);
            s_writer = (uint32_t (*)[3])calloc(s_writer_size / 4 + 1, sizeof *s_writer);
            AddVectoredExceptionHandler(1, writer_crash);
        }
        if (s_writer && addr >= heap && addr < heap + s_writer_size) writer_note(addr, size & ~0x40000000u, sp, fights, audio);
        if (heap && addr >= heap && addr < heap + MEM32(0x331EDCu) && !audio && s_audio_logged < 60) {
            uint32_t k;
            s_audio_logged++;
            fprintf(stderr, "[ROLLBACK] audio heap %08x written by plain code, %u bytes, esp %08x, %s:", addr, size, sp, fights ? "fight thread" : "other thread");
            for (k = 0; k < 64; k++) {
                uint32_t v = MEM32(sp + k * 4);
                if (v >= 0x00011000u && v < 0x00230000u) fprintf(stderr, " %06x", v);
            }
            fputc('\n', stderr);
            fflush(stderr);
        }
    }
    if (s_own_state < 0) return;
    size &= ~0x40000000u;
    if (size > 0x400000u) return;
    if (addr < OWN_RAM) {
        if (addr >= MAIN_STACK_LOW && addr < MAIN_STACK_END) return;
        at = addr;
    } else if (addr >= CONTIG_BASE && addr < CONTIG_BASE + CONTIG_SIZE) at = OWN_RAM + (size_t)(addr - CONTIG_BASE);
    else return;
    /* A byte belongs to whoever wrote it last: heap blocks get recycled, and a mark left by the
     * block's last owner must not outlive it. */
    if (fights && !audio) {
        if (s_own_page[at / PAGE] || s_own_page[(at + size) / PAGE])
            for (i = 0; i < size; i++) s_own[at + i] = 0;
        return;
    }
    for (i = 0; i < size; i++) s_own[at + i] = 1;
    for (i = 0; i <= size / PAGE; i++) s_own_page[at / PAGE + i] = 1;
    s_own_page[(at + size) / PAGE] = 1;
}

/* A block the fight's thread has just been given is the fight's, whoever had it before. */
void rollback_own_clear(uint32_t addr, uint32_t size)
{
    size_t at;
    if (s_own_state <= 0 || size > 0x4000000u) return;
    if (addr < OWN_RAM) at = addr;
    else if (addr >= CONTIG_BASE && addr < CONTIG_BASE + CONTIG_SIZE) at = OWN_RAM + (size_t)(addr - CONTIG_BASE);
    else return;
    memset(s_own + at, 0, size);
}

/* The fight starts: what the loading screen's workers wrote is the fight's data now. Only writes
 * from here on count, and a byte nobody writes cannot differ at a restore anyway. */
void rollback_own_reset(void)
{
    if (s_own_state <= 0) return;
    s_audio_watch = 1;
    memset(s_own, 0, OWN_BYTES);
    memset(s_own_page, 0, OWN_BYTES / PAGE);
}

/* Diagnostics: is the CRT heap's block list (sub_001E9550 walks it) still a list? */
#define GOOD_NODES 96
static uint32_t s_good[GOOD_NODES][4];
static int s_good_count;
static char s_good_when[64];
static long s_good_step;

int rollback_crt_list_check(const char *when, long step)
{
    static int said;
    uint32_t heap = MEM32(0x3C9158u + (MEM32(0x3C9138u) & 0x3F) * 4), start, at;
    long n = 0;
    if (!heap || said) return 0;
    start = at = MEM32(heap + 0x20);
    while (at && n < 200000) {
        at = MEM32(at + 0x10);
        n++;
        if (at == start) {
            /* good: remember it */
            int k = 0;
            at = start;
            do {
                s_good[k][0] = at; s_good[k][1] = MEM32(at + 4); s_good[k][2] = MEM32(at + 0x10); s_good[k][3] = MEM32(at + 0x14);
                at = MEM32(at + 0x10);
                k++;
            } while (at != start && k < GOOD_NODES);
            s_good_count = k;
            s_good_step = step;
            snprintf(s_good_when, sizeof s_good_when, "%s", when);
            return 0;
        }
    }
    if (!at) return 0;
    {
        int k;
        said = 1;
        fprintf(stderr, "[ROLLBACK] heap %08x list lost its head, %s, step %ld; last good %s, step %ld\n", heap, when, step, s_good_when, s_good_step);
        for (k = 0; k < s_good_count; k++) {
            uint32_t a = s_good[k][0];
            size_t own = a < OWN_RAM ? a : OWN_RAM + (size_t)(a - CONTIG_BASE);
            int changed = MEM32(a + 4) != s_good[k][1] || MEM32(a + 0x10) != s_good[k][2] || MEM32(a + 0x14) != s_good[k][3];
            fprintf(stderr, "[ROLLBACK]   %s %08x: size %08x next %08x prev %08x | now %08x %08x %08x | link bytes %s\n",
                    changed ? "CHANGED" : "same   ", a, s_good[k][1], s_good[k][2], s_good[k][3], MEM32(a + 4), MEM32(a + 0x10), MEM32(a + 0x14),
                    s_own_state > 0 && a < 0x84000000u && (s_own[own + 0x10] || s_own[own + 0x14]) ? "NOT the fight's" : "the fight's");
        }
        at = start;
        fprintf(stderr, "[ROLLBACK]   now:");
        for (k = 0; k < 40; k++, at = MEM32(at + 0x10)) fprintf(stderr, " %08x", at);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    return 1;
}

long rollback_own_count(void)
{
    size_t i;
    long n = 0;
    if (s_own_state <= 0) return 0;
    for (i = 0; i < OWN_BYTES / PAGE; i++) n += s_own_page[i];
    return n;
}

/* Guest ranges a restore leaves as they are (rollback_skip): exact bytes, sorted on first use. */
#define MAX_SKIPS 400000
static uint32_t (*s_skip)[2];
static int s_skips, s_skip_sorted;

void rollback_skip(uint32_t first, uint32_t end)
{
    if (!s_skip) s_skip = (uint32_t (*)[2])malloc(MAX_SKIPS * sizeof *s_skip);
    if (s_skip && s_skips < MAX_SKIPS && end > first) {
        s_skip[s_skips][0] = first;
        s_skip[s_skips][1] = end;
        s_skips++;
        s_skip_sorted = 0;
    }
}

static int skip_compare(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void skip_sort(void)
{
    int i, n = 0;
    if (s_skip_sorted || !s_skips) return;
    qsort(s_skip, (size_t)s_skips, sizeof *s_skip, skip_compare);
    for (i = 1; i < s_skips; i++) {
        if (s_skip[i][0] <= s_skip[n][1]) { if (s_skip[i][1] > s_skip[n][1]) s_skip[n][1] = s_skip[i][1]; }
        else { n++; s_skip[n][0] = s_skip[i][0]; s_skip[n][1] = s_skip[i][1]; }
    }
    s_skips = n + 1;
    s_skip_sorted = 1;
}

/* The first skip range ending after `at`, or s_skips. */
static int skip_after(uint32_t at)
{
    int lo = 0, hi = s_skips;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s_skip[mid][1] <= at) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int skip_covers(uint32_t addr)
{
    int i;
    if (!s_skips) return 0;
    skip_sort();
    i = skip_after(addr);
    return i < s_skips && s_skip[i][0] <= addr;
}

static int skipped(uint32_t page)
{
    int i;
    skip_sort();
    i = skip_after(page);
    return i < s_skips && s_skip[i][0] < page + PAGE;
}

/* Put a page back, all but its skipped bytes. Only the other bytes are ever touched, so a
 * thread working in the skipped ones never sees them change. */
static void restore_page(unsigned char *now, const unsigned char *saved, uint32_t guest, size_t len)
{
    uint32_t at = guest, end = guest + (uint32_t)len;
    int i = skip_after(at);
    while (at < end) {
        uint32_t stop = i < s_skips && s_skip[i][0] < end ? s_skip[i][0] : end;
        if (stop > at) {
            size_t own = at < 0x80000000u ? at : OWN_RAM + (size_t)(at - CONTIG_BASE);
            if (s_own_state > 0 && at < 0x84000000u && s_own_page[own / PAGE]) {
                uint32_t k;
                for (k = at; k < stop; k++, own++)
                    if (!s_own[own]) now[k - guest] = saved[k - guest];
            } else memcpy(now + (at - guest), saved + (at - guest), stop - at);
        }
        if (stop == end) break;
        at = s_skip[i][1];
        i++;
    }
}

/* Only the pages that changed are written back, so a restore touches what a fight changed
 * and nothing else. */
void rollback_restore(const RollbackState *s)
{
    unsigned char *now = (unsigned char *)XBOX_PTR(GUEST_BASE);
    size_t off;
    if (!s || !s->ok) return;
    skip_sort();
    for (off = 0; off < s->size; off += PAGE) {
        size_t len = s->size - off < PAGE ? s->size - off : PAGE;
        if (memcmp(now + off, s->memory + off, len))
            restore_page(now + off, s->memory + off, GUEST_BASE + (uint32_t)off, len);
    }
    now = (unsigned char *)XBOX_PTR(CONTIG_BASE);
    for (off = 0; off < CONTIG_SIZE; off += PAGE)
        if (memcmp(now + off, s->contig + off, PAGE))
            restore_page(now + off, s->contig + off, CONTIG_BASE + (uint32_t)off, PAGE);
    regs_report(s);
    regs_restore(s);
    walk((RollbackState *)s, 1);
}

/* Every changed byte span (closer than 16 bytes apart counts as one), for working out whose
 * memory a page is. */
void rollback_dump_changes(const RollbackState *s, FILE *out)
{
    const unsigned char *now = (const unsigned char *)XBOX_PTR(GUEST_BASE);
    size_t off, i;
    for (off = 0; off < s->size; off += PAGE) {
        size_t len = s->size - off < PAGE ? s->size - off : PAGE, start = 0, last = 0;
        int open = 0, spans = 0;
        if (!memcmp(now + off, s->memory + off, len)) continue;
        fprintf(out, "[ROLLBACK]   page %08x:", GUEST_BASE + (uint32_t)off);
        for (i = 0; i < len; i++) {
            if (now[off + i] == s->memory[off + i]) continue;
            if (open && i - last <= 16) { last = i; continue; }
            if (open && spans < 24) fprintf(out, " %x+%x", GUEST_BASE + (uint32_t)(off + start), (unsigned)(last - start + 1));
            if (open) spans++;
            open = 1;
            start = last = i;
        }
        if (open && spans < 24) fprintf(out, " %x+%x", GUEST_BASE + (uint32_t)(off + start), (unsigned)(last - start + 1));
        if (spans >= 24) fprintf(out, " ... (%d spans)", spans + 1);
        fputc('\n', out);
    }
    now = (const unsigned char *)XBOX_PTR(CONTIG_BASE);
    {
        size_t run_start = 0, run_bytes = 0;
        int in_run = 0;
        for (off = 0; off <= CONTIG_SIZE; off += PAGE) {
            size_t diff = 0;
            if (off < CONTIG_SIZE && memcmp(now + off, s->contig + off, PAGE))
                for (i = 0; i < PAGE; i++) diff += now[off + i] != s->contig[off + i];
            if (diff) {
                if (!in_run) { in_run = 1; run_start = off; run_bytes = 0; }
                run_bytes += diff;
            } else if (in_run) {
                fprintf(out, "[ROLLBACK]   window %08x..%08x: %zu pages, %zu bytes changed\n",
                        CONTIG_BASE + (uint32_t)run_start, CONTIG_BASE + (uint32_t)off, (off - run_start) / PAGE, run_bytes);
                in_run = 0;
            }
        }
    }
}

int rollback_compare(const RollbackState *s, uint32_t (*ranges)[2], int max, int *range_count)
{
    const unsigned char *now = (const unsigned char *)XBOX_PTR(GUEST_BASE);
    size_t off;
    int pages = 0, n = 0, open = 0;
    for (off = 0; off < s->size; off += PAGE) {
        size_t len = s->size - off < PAGE ? s->size - off : PAGE;
        int differs = memcmp(now + off, s->memory + off, len) != 0;
        if (differs) {
            pages++;
            if (open && n > 0 && ranges[n - 1][1] == GUEST_BASE + (uint32_t)off) ranges[n - 1][1] += (uint32_t)len;
            else if (n < max) {
                ranges[n][0] = GUEST_BASE + (uint32_t)off;
                ranges[n][1] = GUEST_BASE + (uint32_t)(off + len);
                n++;
                open = 1;
            } else open = 0;
        } else open = 0;
    }
    now = (const unsigned char *)XBOX_PTR(CONTIG_BASE);
    open = 0;
    for (off = 0; off < CONTIG_SIZE; off += PAGE) {
        if (memcmp(now + off, s->contig + off, PAGE)) {
            pages++;
            if (open && ranges[n - 1][1] == CONTIG_BASE + (uint32_t)off) ranges[n - 1][1] += PAGE;
            else if (n < max) {
                ranges[n][0] = CONTIG_BASE + (uint32_t)off;
                ranges[n][1] = CONTIG_BASE + (uint32_t)off + PAGE;
                n++;
                open = 1;
            } else open = 0;
        } else open = 0;
    }
    if (range_count) *range_count = n;
    return pages;
}

/* ---- What a fight step writes (rollback_track_begin / _end) ---------------------------
 * For the length of one step the guest memory's writable pages are made read-only. The
 * first write to each then faults: the handler keeps a copy of the page as it was and which
 * thread wrote it, and gives the page its access back. At the end of the step each such page
 * is compared with its copy: the bytes the step's own thread changed are marked as the
 * fight's, for good (a bit per guest byte). Pages first written by another thread (sound,
 * USB, the GPU's executor) are only counted. A restore with a mask puts back the marked
 * bytes and nothing else, so rendering, sound and the devices are never rewound. */
#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>

#define MAX_HITS    4096
#define MAX_PAUSED  256

static unsigned char *s_mask;           /* a bit per guest byte */
static unsigned char *s_pre;            /* MAX_HITS page copies */
static int *s_hit_of;                   /* page -> hit index, -1 none */
static uint32_t s_hit_page[MAX_HITS];
static DWORD s_hit_thread[MAX_HITS];
static int s_hits;
static volatile LONG s_tracking;
static DWORD s_track_thread;
static CRITICAL_SECTION s_track_lock;
static int s_track_ready;
static uint32_t s_regions[256][2];      /* host offsets made read-only, to give back */
static int s_region_count;
static long s_other_pages, s_track_steps, s_mask_bytes;
/* RECOMP_ROLLBACK_PAUSE=1: the process's other threads are suspended while a step is tracked,
 * so every write is the step's own (a learning pass: slower, and only safe because the step
 * takes no lock another thread holds). */
static HANDLE s_paused[MAX_PAUSED];
static int s_paused_count, s_pause = -1;

static void pause_others(void)
{
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap;
    THREADENTRY32 te;
    s_paused_count = 0;
    if (s_pause < 0) s_pause = getenv("RECOMP_ROLLBACK_PAUSE") != NULL;
    if (!s_pause) return;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            HANDLE h;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || s_paused_count == MAX_PAUSED) continue;
            h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
            s_paused[s_paused_count++] = h;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void resume_others(void)
{
    int i;
    for (i = 0; i < s_paused_count; i++) {
        ResumeThread(s_paused[i]);
        CloseHandle(s_paused[i]);
    }
    s_paused_count = 0;
}

static LONG CALLBACK track_fault(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    unsigned char *base = (unsigned char *)XBOX_PTR(GUEST_BASE);
    size_t size = xbox_GetMappedSize();
    uintptr_t at;
    size_t page;
    DWORD old;
    if (!s_tracking || r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2
        || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    at = (uintptr_t)r->ExceptionInformation[1];
    if (at < (uintptr_t)base || at >= (uintptr_t)base + size) return EXCEPTION_CONTINUE_SEARCH;
    page = (at - (uintptr_t)base) / PAGE;
    EnterCriticalSection(&s_track_lock);
    if (s_hit_of[page] < 0 && s_hits < MAX_HITS) {
        int i = s_hits++;
        memcpy(s_pre + (size_t)i * PAGE, base + page * PAGE, PAGE);
        s_hit_page[i] = (uint32_t)page;
        s_hit_thread[i] = GetCurrentThreadId();
        s_hit_of[page] = i;
    }
    VirtualProtect(base + page * PAGE, PAGE, PAGE_READWRITE, &old);
    LeaveCriticalSection(&s_track_lock);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int track_init(void)
{
    size_t size = xbox_GetMappedSize(), pages = size / PAGE + 1, i;
    if (s_track_ready) return s_track_ready > 0;
    s_track_ready = -1;
    s_mask = (unsigned char *)calloc(size / 8 + 1, 1);
    s_pre = (unsigned char *)malloc((size_t)MAX_HITS * PAGE);
    s_hit_of = (int *)malloc(pages * sizeof *s_hit_of);
    if (!s_mask || !s_pre || !s_hit_of) return 0;
    for (i = 0; i < pages; i++) s_hit_of[i] = -1;
    InitializeCriticalSection(&s_track_lock);
    if (!AddVectoredExceptionHandler(1, track_fault)) return 0;
    s_track_ready = 1;
    return 1;
}

void rollback_track_begin(void)
{
    unsigned char *base = (unsigned char *)XBOX_PTR(GUEST_BASE), *at = base;
    size_t size = xbox_GetMappedSize();
    if (!track_init()) return;
    s_region_count = 0;
    s_track_thread = GetCurrentThreadId();
    pause_others();
    InterlockedExchange(&s_tracking, 1);
    while (at < base + size && s_region_count < 256) {
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *end;
        DWORD old;
        if (!VirtualQuery(at, &mbi, sizeof mbi)) break;
        end = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (end > base + size) end = base + size;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE
            && VirtualProtect(at, (size_t)(end - at), PAGE_READONLY, &old)) {
            s_regions[s_region_count][0] = (uint32_t)(at - base);
            s_regions[s_region_count][1] = (uint32_t)(end - base);
            s_region_count++;
        }
        at = end;
    }
}

void rollback_track_end(void)
{
    unsigned char *base = (unsigned char *)XBOX_PTR(GUEST_BASE);
    int i, j;
    if (s_track_ready <= 0 || !s_tracking) return;
    for (i = 0; i < s_region_count; i++) {
        DWORD old;
        VirtualProtect(base + s_regions[i][0], s_regions[i][1] - s_regions[i][0], PAGE_READWRITE, &old);
    }
    InterlockedExchange(&s_tracking, 0);
    resume_others();
    EnterCriticalSection(&s_track_lock);
    for (i = 0; i < s_hits; i++) {
        size_t page = s_hit_page[i];
        const unsigned char *was = s_pre + (size_t)i * PAGE, *now = base + page * PAGE;
        s_hit_of[page] = -1;
        if (s_hit_thread[i] != s_track_thread) { s_other_pages++; continue; }
        for (j = 0; j < (int)PAGE; j++) {
            size_t byte = page * PAGE + (size_t)j;
            if (was[j] != now[j] && !(s_mask[byte / 8] & (1u << (byte % 8)))) {
                s_mask[byte / 8] |= (unsigned char)(1u << (byte % 8));
                s_mask_bytes++;
            }
        }
    }
    s_hits = 0;
    s_track_steps++;
    LeaveCriticalSection(&s_track_lock);
}

/* ---- Who writes a guest address (rollback_watch), for diagnostics ----------------------
 * The address's page goes read-only; a write to it faults, is let through one instruction at a
 * time, and a write to a watched address itself is logged with the code addresses on the guest
 * stack. */
#define MAX_WATCH 8
static uint32_t s_watch[MAX_WATCH];
static int s_watches, s_watch_logged;
static DWORD s_watch_tls = TLS_OUT_OF_INDEXES;

static void watch_protect(void)
{
    int i;
    DWORD old;
    for (i = 0; i < s_watches; i++)
        VirtualProtect((void *)XBOX_PTR(s_watch[i] & ~(PAGE - 1)), PAGE, PAGE_READONLY, &old);
}

static LONG CALLBACK watch_fault(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && TlsGetValue(s_watch_tls)) {
        uintptr_t was = (uintptr_t)TlsGetValue(s_watch_tls);
        TlsSetValue(s_watch_tls, NULL);
        if (was != 1 && s_watch_logged < 400) {
            uint32_t g = (uint32_t)(was - (uintptr_t)XBOX_PTR(0));
            fprintf(stderr, "[ROLLBACK]   now %08x = %08x\n", g & ~3u, MEM32(g & ~3u));
            fflush(stderr);
        }
        watch_protect();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1) {
        uintptr_t at = (uintptr_t)r->ExceptionInformation[1];
        int i, hit = 0, page = 0;
        DWORD old;
        for (i = 0; i < s_watches; i++) {
            uintptr_t w = (uintptr_t)XBOX_PTR(s_watch[i]);
            if (at >= (w & ~(uintptr_t)(PAGE - 1)) && at < (w & ~(uintptr_t)(PAGE - 1)) + PAGE) page = 1;
            if (at + 4 > w && at < w + 4) hit = 1;
        }
        if (!page) return EXCEPTION_CONTINUE_SEARCH;
        VirtualProtect((void *)(at & ~(uintptr_t)(PAGE - 1)), PAGE, PAGE_READWRITE, &old);
        if (hit && s_watch_logged++ < 400) {
            uint32_t sp = (uint32_t)g_esp, k;
            fprintf(stderr, "[ROLLBACK] write to %08x, was %08x, rva %llx, esp %08x, code on the stack:",
                    (uint32_t)(at - (uintptr_t)XBOX_PTR(0)), MEM32((uint32_t)(at - (uintptr_t)XBOX_PTR(0)) & ~3u),
                    (unsigned long long)((uintptr_t)e->ContextRecord->Rip - (uintptr_t)GetModuleHandleA(NULL)), sp);
            for (k = 0; k < 96; k++) {
                uint32_t v = MEM32(sp + k * 4);
                if (v >= 0x00011000u && v < 0x00230000u) fprintf(stderr, " %06x", v);
            }
            fputc('\n', stderr);
            if (s_watch_logged == 1) {
                fprintf(stderr, "[ROLLBACK]   stack:");
                for (k = 0; k < 48; k++) fprintf(stderr, "%s %08x", k % 8 ? "" : "\n[ROLLBACK]    ", MEM32(sp + k * 4));
                fprintf(stderr, "\n[ROLLBACK]   rax %llx rcx %llx rdx %llx rbx %llx rsi %llx rdi %llx r8 %llx r9 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n",
                        e->ContextRecord->Rax, e->ContextRecord->Rcx, e->ContextRecord->Rdx, e->ContextRecord->Rbx, e->ContextRecord->Rsi,
                        e->ContextRecord->Rdi, e->ContextRecord->R8, e->ContextRecord->R9, e->ContextRecord->R12, e->ContextRecord->R13,
                        e->ContextRecord->R14, e->ContextRecord->R15);
            }
            fflush(stderr);
        }
        TlsSetValue(s_watch_tls, hit ? (void *)at : (void *)1);
        e->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- Whose memory is which (rollback_learn_step), for working out the skip list -----------
 * RECOMP_ROLLBACK_LEARN=<first step>,<samples>: every 5th step from there the fight's thread
 * stands still for a moment. Whatever changes meanwhile cannot be the fight's: it is another
 * thread's stack or data (sound, streaming, the GPU's), which a rewind must leave alone. The
 * pages are written to RECOMP_ROLLBACK_LEARN_OUT, in RECOMP_ROLLBACK_SKIPFILE's format. */
#define LEARN_RAM_BYTES 0x04000000u
#define LEARN_BYTES (LEARN_RAM_BYTES + CONTIG_SIZE)
#define LEARN_WHOLE_PAGE 128u          /* bytes changed in a page that make it another thread's */

static void learn_report(const unsigned char *others)
{
    const char *name = getenv("RECOMP_ROLLBACK_LEARN_OUT");
    FILE *f = fopen(name ? name : "rollback-learn.txt", "w");
    size_t page;
    int spans = 0, whole = 0;
    if (!f) return;
    for (page = 0; page < LEARN_BYTES / PAGE; page++) {
        const unsigned char *b = others + page * PAGE;
        uint32_t base = page < LEARN_RAM_BYTES / PAGE ? GUEST_BASE + (uint32_t)(page * PAGE) : CONTIG_BASE + (uint32_t)(page * PAGE - LEARN_RAM_BYTES);
        unsigned i, count = 0;
        for (i = 0; i < PAGE; i++) count += b[i];
        if (!count) continue;
        if (count >= LEARN_WHOLE_PAGE) {
            fprintf(f, "%08x %08x\n", base, base + PAGE);
            whole++;
            continue;
        }
        for (i = 0; i < PAGE; i++) {
            unsigned j;
            if (!b[i]) continue;
            /* whole dwords: a value half rewound is no value at all */
            j = i & ~3u;
            i |= 3u;
            while (i + 1 < PAGE && b[i + 1]) i++;
            i |= 3u;
            fprintf(f, "%08x %08x\n", base + j, base + i + 1);
            spans++;
        }
    }
    fclose(f);
    fprintf(stderr, "[ROLLBACK] learned: %d whole pages and %d byte spans written by other threads\n", whole, spans);
}

void rollback_learn_step(long step)
{
    static int parsed;
    static long first = -1, samples, taken;
    static unsigned char *before_ram, *before_win, *others;
    unsigned char *ram = (unsigned char *)XBOX_PTR(GUEST_BASE), *win = (unsigned char *)XBOX_PTR(CONTIG_BASE);
    size_t ram_size = xbox_GetMappedSize(), off, i;
    if (!parsed) {
        const char *v = getenv("RECOMP_ROLLBACK_LEARN");
        parsed = 1;
        if (v && sscanf(v, "%ld,%ld", &first, &samples) == 2) {
            before_ram = (unsigned char *)malloc(ram_size);
            before_win = (unsigned char *)malloc(CONTIG_SIZE);
            others = (unsigned char *)calloc(LEARN_BYTES, 1);
            if (!before_ram || !before_win || !others) first = -1;
        } else first = -1;
    }
    if (first < 0 || step < first || (step - first) % 5) return;
    memcpy(before_ram, ram, ram_size);
    memcpy(before_win, win, CONTIG_SIZE);
    Sleep(30);
    for (off = 0; off < ram_size; off += PAGE)
        if (memcmp(ram + off, before_ram + off, PAGE))
            for (i = 0; i < PAGE; i++) if (ram[off + i] != before_ram[off + i]) others[off + i] = 1;
    for (off = 0; off < CONTIG_SIZE; off += PAGE)
        if (memcmp(win + off, before_win + off, PAGE))
            for (i = 0; i < PAGE; i++) if (win[off + i] != before_win[off + i]) others[LEARN_RAM_BYTES + off + i] = 1;
    if (++taken >= samples) {
        learn_report(others);
        first = -1;
    }
}

/* ---- The sound engine's own memory (rollback_sound_alloc) -----------------------------------
 * CSIS, the sound engine, allocates through an IAllocator the game installs (sub_0001DC10) out of
 * the game's heap, from the sound thread as well as the fight's. A rewind cannot undo that heap's
 * books for one thread and not the other, so with rollback on the sound engine has an area of
 * its own, which no rewind touches. */
#define SOUND_ARENA_SIZE (3u * 1024u * 1024u)
#define ARENA_HEADER     16u
extern uint32_t xbox_ContiguousAllocEx(uint32_t size, uint32_t alignment, uint32_t low, uint32_t high);
static uint32_t s_arena, s_arena_free, s_arena_peak, s_arena_used;
static int s_arena_state;          /* 0 not decided, 1 on, -1 off */
static CRITICAL_SECTION s_arena_lock;

static int arena_ready(void)
{
    if (!s_arena_state) {
        s_arena_state = -1;
        if (getenv("RECOMP_ROLLBACK_TEST")) {
            s_arena = xbox_ContiguousAllocEx(SOUND_ARENA_SIZE, 4096, 0, 0xFFFFFFFFu);
            if (s_arena) {
                InitializeCriticalSection(&s_arena_lock);
                s_arena_free = s_arena;
                MEM32(s_arena) = SOUND_ARENA_SIZE;
                MEM32(s_arena + 4) = 0;
                rollback_skip(s_arena, s_arena + SOUND_ARENA_SIZE);
                s_arena_state = 1;
                fprintf(stderr, "[ROLLBACK] sound engine area at %08x\n", s_arena);
            }
        }
    }
    return s_arena_state > 0;
}

static int s_arena_state_get(void) { return s_arena_state; }
static int rollback_in_arena(uint32_t addr) { return addr >= s_arena && addr < s_arena + SOUND_ARENA_SIZE; }

/* First fit over a free list kept in the blocks: size, next. */
uint32_t rollback_sound_alloc(uint32_t size)
{
    uint32_t at, prev = 0, need, result = 0;
    if (!arena_ready()) return 0;
    need = (size + ARENA_HEADER + 15u) & ~15u;
    EnterCriticalSection(&s_arena_lock);
    for (at = s_arena_free; at; prev = at, at = MEM32(at + 4)) {
        uint32_t have = MEM32(at), next = MEM32(at + 4), keep;
        if (have < need) continue;
        if (have - need >= ARENA_HEADER * 2) {
            keep = at + need;
            MEM32(keep) = have - need;
            MEM32(keep + 4) = next;
            MEM32(at) = need;
        } else {
            keep = next;
            need = have;
        }
        if (prev) MEM32(prev + 4) = keep;
        else s_arena_free = keep;
        MEM32(at + 4) = 0x534E4421u;
        result = at + ARENA_HEADER;
        s_arena_used += need;
        if (s_arena_used > s_arena_peak) s_arena_peak = s_arena_used;
        break;
    }
    LeaveCriticalSection(&s_arena_lock);
    if (!result) fprintf(stderr, "[ROLLBACK] sound engine area full (%u bytes used, %u wanted)\n", s_arena_used, size);
    return result;
}

int rollback_sound_free(uint32_t addr)
{
    uint32_t block;
    if (s_arena_state <= 0 || addr < s_arena || addr >= s_arena + SOUND_ARENA_SIZE) return 0;
    block = addr - ARENA_HEADER;
    EnterCriticalSection(&s_arena_lock);
    if (MEM32(block + 4) == 0x534E4421u) {
        s_arena_used -= MEM32(block);
        /* sorted by address, joined to its neighbours */
        {
            uint32_t prev = 0, at = s_arena_free;
            while (at && at < block) { prev = at; at = MEM32(at + 4); }
            MEM32(block + 4) = at;
            if (at && block + MEM32(block) == at) {
                MEM32(block) += MEM32(at);
                MEM32(block + 4) = MEM32(at + 4);
            }
            if (prev) {
                MEM32(prev + 4) = block;
                if (prev + MEM32(prev) == block) {
                    MEM32(prev) += MEM32(block);
                    MEM32(prev + 4) = MEM32(block + 4);
                }
            } else s_arena_free = block;
        }
    }
    LeaveCriticalSection(&s_arena_lock);
    return 1;
}

/* Frees other threads make of the game heap's blocks during a fight wait for the fight's thread
 * (rollback_deferred_pop): the heap's books are the fight's, rewound with it. */
#define MAX_DEFERRED 4096
static uint32_t s_deferred[MAX_DEFERRED];
static int s_deferred_count;

int rollback_defer_free(uint32_t addr)
{
    int ok = 0;
    if (!arena_ready()) return 0;
    EnterCriticalSection(&s_arena_lock);
    if (s_deferred_count < MAX_DEFERRED) { s_deferred[s_deferred_count++] = addr; ok = 1; }
    LeaveCriticalSection(&s_arena_lock);
    return ok;
}

uint32_t rollback_deferred_pop(void)
{
    uint32_t a = 0;
    if (s_arena_state <= 0 || !s_deferred_count) return 0;
    EnterCriticalSection(&s_arena_lock);
    if (s_deferred_count) a = s_deferred[--s_deferred_count];
    LeaveCriticalSection(&s_arena_lock);
    return a;
}

/* The sound system's fixed blocks (sub_0001D880 records them in these globals). */
void rollback_skip_audio(void)
{
    static const uint32_t blocks[][2] = {{0x331EC4u, 0x331EDCu}, {0x331D44u, 0}, {0x331D1Cu, 0x40000u}, {0x331DA4u, 0x2800u}};
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t at = MEM32(blocks[i][0]);
        uint32_t size = blocks[i][1] > 0x400000u || (blocks[i][1] & 0xFF000000u) ? 0 : blocks[i][1];
        if (blocks[i][1] == 0x331EDCu) size = MEM32(0x331EDCu);
        if (!at) continue;
        /* The AEMS stream buffer: its size is the game's setting, and the next block follows it. */
        if (!size) size = MEM32(0x331D1Cu) > at ? MEM32(0x331D1Cu) - at : 0;
        fprintf(stderr, "[ROLLBACK] audio block %08x, %u bytes\n", at, size);
        rollback_skip(at, at + size);
    }
    {
        /* The stream library's request table: 32 entries, each with its own queues. */
        uint32_t table = MEM32(0x3C8F30u);
        if (table) {
            fprintf(stderr, "[ROLLBACK] stream request table %08x\n", table);
            rollback_skip(table, table + 32u * 0xB8u);
        }
    }
    fprintf(stderr, "[ROLLBACK] sound engine area: %u bytes used, peak %u\n", s_arena_used, s_arena_peak);
}

void rollback_watch(uint32_t guest)
{
    if (s_watch_tls == TLS_OUT_OF_INDEXES) {
        s_watch_tls = TlsAlloc();
        AddVectoredExceptionHandler(1, watch_fault);
    }
    if (s_watches < MAX_WATCH) s_watch[s_watches++] = guest;
    watch_protect();
}

void rollback_track_stats(long *steps, long *fight_bytes, long *other_pages)
{
    *steps = s_track_steps;
    *fight_bytes = s_mask_bytes;
    *other_pages = s_other_pages;
}

void rollback_restore_fight(const RollbackState *s)
{
    unsigned char *now = (unsigned char *)XBOX_PTR(GUEST_BASE);
    size_t byte, size;
    if (!s || !s->ok || !s_mask) return;
    size = s->size;
    for (byte = 0; byte < size; byte += 8) {
        unsigned bits = s_mask[byte / 8];
        int k;
        if (!bits || skipped(GUEST_BASE + (uint32_t)(byte & ~(size_t)(PAGE - 1)))) continue;
        for (k = 0; k < 8 && byte + (size_t)k < size; k++)
            if (bits & (1u << k)) now[byte + (size_t)k] = s->memory[byte + (size_t)k];
    }
    regs_report(s);
    regs_restore(s);
    walk((RollbackState *)s, 1);
}

/* The fight's bytes, grouped in ranges no closer than `gap` apart (diagnostics). */
void rollback_dump_mask(FILE *out, size_t gap)
{
    size_t size = xbox_GetMappedSize(), byte, start = 0, last = 0;
    int open = 0, n = 0;
    if (!s_mask) return;
    for (byte = 0; byte < size; byte++) {
        if (!(s_mask[byte / 8] & (1u << (byte % 8)))) continue;
        if (open && byte - last <= gap) { last = byte; continue; }
        if (open && n++ < 200) fprintf(out, "[ROLLBACK]   fight bytes %08x..%08x\n", GUEST_BASE + (uint32_t)start, GUEST_BASE + (uint32_t)last + 1);
        open = 1;
        start = last = byte;
    }
    if (open && n++ < 200) fprintf(out, "[ROLLBACK]   fight bytes %08x..%08x\n", GUEST_BASE + (uint32_t)start, GUEST_BASE + (uint32_t)last + 1);
    fprintf(out, "[ROLLBACK]   %d ranges\n", n);
}
#else
void rollback_track_begin(void) {}
void rollback_track_end(void) {}
void rollback_watch(uint32_t guest) { (void)guest; }
uint32_t rollback_sound_alloc(uint32_t size) { (void)size; return 0; }
int rollback_sound_free(uint32_t addr) { (void)addr; return 0; }
void rollback_skip_audio(void) {}
void rollback_sound_begin(void) {}
int rollback_heapfix(void) { return 0; }
void rollback_lib_enter(void) {}
void rollback_lib_leave(void) {}
int rollback_in_library(void) { return 0; }
int rollback_sound_end(uint32_t (*out)[2], int *count) { (void)out; (void)count; return 0; }
int rollback_defer_free(uint32_t addr) { (void)addr; return 0; }
uint32_t rollback_deferred_pop(void) { return 0; }
void rollback_learn_step(long step) { (void)step; }
void rollback_track_stats(long *steps, long *fight_bytes, long *other_pages) { *steps = *fight_bytes = *other_pages = 0; }
void rollback_restore_fight(const RollbackState *s) { rollback_restore(s); }
void rollback_dump_mask(FILE *out, size_t gap) { (void)out; (void)gap; }
#endif
