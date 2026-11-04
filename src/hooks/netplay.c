/*
 * Netplay sessions (docs/netplay.md).
 *
 * Up to four players meet in the host's lobby over TCP, agree on a match, and every
 * game starts it in lockstep mode (lockstep.c), where the same inputs play the same
 * fight. The host is the hub: every joiner has one connection, to the host, and the
 * host passes each joiner's messages on to the others.
 *
 *   1. a joiner sends  HELLO <protocol> <version> <program> <game data> <name>
 *      and the host answers WELCOME <player> or REFUSE <reason>: a different build
 *      or a different dump would play a different fight;
 *   2. in the lobby the host sends LOBBY (the players, their ready flags, the phase
 *      and the match so far) whenever it changes; a joiner sends READYUP <0|1> and,
 *      once the host has moved the lobby to picking, PICK <fighter> [<profile>] for
 *      its own slot (a profile with fighter 99: that profile's created fighter);
 *   3. the host starts: MATCH <seed> <type> <rules> <venue> <slots> <fighters>
 *      <teams> <owners> <profiles>. A joiner with a created fighter sends its profile's
 *      save (FILE <folder> <name> <size>, DATA <hex>..., END), and every joiner then
 *      SENT; the host sends every joiner the created fighters' profiles the same way,
 *      then FILES. Each game adds the others' to its player's own saves for the
 *      session (place_saves), and answers READY; the host sends GO. The launcher (or
 *      a scripted run) starts the game, which goes straight from the main menu into
 *      that match through the front end's own commands (fe_inject.c);
 *   4. in the match: AT <player> <step> (the start barrier), IN <player> <step>
 *      <input> every step, SYNC <player> <step> <hash> (the desync check), and from
 *      the host GONE <player> <step> when a player has left: from that step on every
 *      game plays that player's slot with no input, so they all still agree. A step
 *      on the wire carries its match too (match << 24 | step), so a rematch never
 *      takes the last match's messages for its own;
 *   5. after a match: VOTE <player> <match> <1 rematch | 0 quit>. A rematch waits
 *      until every player has chosen it, and then starts on every game; one quit (or
 *      a player gone) sends everyone who chose it back to the menus instead.
 *
 * Names travel as written, profile names in hex (they may have spaces); "-" is none.
 * Messages are text lines. The worker thread owns the sockets; the game and the
 * launcher read the state under a lock, and sends go through a second lock because
 * the game thread sends its inputs itself.
 */
#define _CRT_SECURE_NO_WARNINGS
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#define PATH_SEP "\\"
#define make_dir(path) _mkdir(path)
#define remove_dir(path) _rmdir(path)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#define PATH_SEP "/"
#define make_dir(path) mkdir(path, 0777)
#define remove_dir(path) rmdir(path)
#endif
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_start.h"
#include "netplay.h"
#include "netplay_router.h"
#include "recomp/gen/recomp_types.h"
#include "test_observations.h"

#ifndef DEFJAM_VERSION
#define DEFJAM_VERSION "dev"
#endif
#define PROTOCOL        2
#define MAXP            NETPLAY_MAX_PLAYERS
#define BARRIER_MS      120000      /* how long a game waits for the others at the first step */
#define AGREE_MS        15000       /* how long the host waits for every READY */

/* The match a lobby opens with: One on One at the Foundation, the harness's fighters
 * (and the Free For All route's two CPUs for slots 2 and 3). */
#define DEFAULT_VENUE   1
static const int s_default_fighter[MAXP] = {56, 55, 22, 35};

/* ---- Platform: locks, a thread, sleeping, sockets -------------------------------- */
#if defined(_WIN32)
static CRITICAL_SECTION s_lock, s_send_lock;
static void lock_init(void) { InitializeCriticalSection(&s_lock); InitializeCriticalSection(&s_send_lock); }
static void lock(void) { EnterCriticalSection(&s_lock); }
static void unlock(void) { LeaveCriticalSection(&s_lock); }
static void send_lock(void) { EnterCriticalSection(&s_send_lock); }
static void send_unlock(void) { LeaveCriticalSection(&s_send_lock); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static DWORD WINAPI worker_entry(LPVOID arg);
static int start_thread(void) { return CreateThread(NULL, 0, worker_entry, NULL, 0, NULL) != NULL; }
static int net_init(void) { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }
static uint32_t fresh_seed(void) { return (uint32_t)GetTickCount() ^ (uint32_t)GetCurrentProcessId() * 2654435761u; }
#else
static pthread_mutex_t s_lock_m = PTHREAD_MUTEX_INITIALIZER, s_send_m = PTHREAD_MUTEX_INITIALIZER;
static void lock_init(void) {}
static void lock(void) { pthread_mutex_lock(&s_lock_m); }
static void unlock(void) { pthread_mutex_unlock(&s_lock_m); }
static void send_lock(void) { pthread_mutex_lock(&s_send_m); }
static void send_unlock(void) { pthread_mutex_unlock(&s_send_m); }
static void sleep_ms(int ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static void *worker_entry(void *arg);
static int start_thread(void) { pthread_t t; return pthread_create(&t, NULL, worker_entry, NULL) == 0; }
static int net_init(void) { return 1; }
static uint32_t fresh_seed(void) { return (uint32_t)time(NULL) ^ (uint32_t)getpid() * 2654435761u; }
#endif

/* ---- Session state (under the lock) ------------------------------------------- */
/* The locks exist from the first session on; before it there is no worker thread, and
 * the launcher reads the state every frame, so the readers skip them until then. */
static int s_inited;
static volatile int s_cancel;
static NetplayState s_state = NETPLAY_IDLE;
static char s_status[160];
static NetplayLobby s_lobby;        /* the host's lobby, or a joiner's copy of it */
static NetplayMatch s_match;        /* the agreed match */
static int s_hosting;
static uint16_t s_port;
static char s_address[128];
static char s_name[NETPLAY_NAME_LEN] = "Player";
static int s_lobby_dirty, s_pick_dirty, s_start_request, s_ready_dirty, s_ready_value;
static char s_pick_profile[NETPLAY_PROFILE_LEN];
static int s_wait_players = 2;      /* scripted host: start when this many are in */
static char s_fp_program[24], s_fp_data[24];

/* The connections. On the host, s_peer[p] is player p's (none for 0, the host); on a
 * joiner, s_peer[0] is the host's. */
static sock_t s_peer[MAXP] = {SOCK_BAD, SOCK_BAD, SOCK_BAD, SOCK_BAD};
static char s_in[MAXP][1024];
static size_t s_in_len[MAXP];

static void set_state(NetplayState state, const char *format, ...)
{
    va_list args;
    if (s_inited) lock();
    s_state = state;
    va_start(args, format);
    vsnprintf(s_status, sizeof s_status, format, args);
    va_end(args);
    if (s_inited) unlock();
    fprintf(stderr, "[NETPLAY] %s\n", s_status);
    fflush(stderr);
}

NetplayState netplay_state(void)
{
    NetplayState state;
    if (s_inited) lock();
    state = s_state;
    if (s_inited) unlock();
    return state;
}

void netplay_status(char *out, int size)
{
    if (s_inited) lock();
    snprintf(out, (size_t)size, "%s", s_status);
    if (s_inited) unlock();
}

int netplay_lobby(NetplayLobby *out)
{
    int ok;
    if (s_inited) lock();
    *out = s_lobby;
    ok = s_lobby.local_player >= 0 && (s_state == NETPLAY_LISTENING || s_state == NETPLAY_LOBBY
                                       || s_state == NETPLAY_READY);
    if (s_inited) unlock();
    return ok;
}

int netplay_match(NetplayMatch *out)
{
    int ok;
    if (s_inited) lock();
    ok = s_state == NETPLAY_READY;
    if (ok) *out = s_match;
    if (s_inited) unlock();
    return ok;
}

/* ---- Fingerprints: this program and the game data -------------------------------- */
static int file_fingerprint(const char *path, char *out, size_t size)
{
    unsigned char buf[65536];
    uint64_t h = 1469598103934665603ull;
    size_t n, i;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(out, size, "none"); return 0; }
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (i = 0; i < n; i++)
            h = (h ^ buf[i]) * 1099511628211ull;
    fclose(f);
    snprintf(out, size, "%016llx", (unsigned long long)h);
    return 1;
}

static void fingerprints(void)
{
    char exe[1024] = "";
#if defined(_WIN32)
    GetModuleFileNameA(NULL, exe, sizeof exe);
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) exe[n] = 0;
#endif
    file_fingerprint(exe, s_fp_program, sizeof s_fp_program);
    file_fingerprint("game/default.xbe", s_fp_data, sizeof s_fp_data);
}

/* A name as it travels: letters, digits, '-' and '_' only, never empty. */
static void clean_name(char *out, const char *in)
{
    int i, n = 0;
    for (i = 0; in && in[i] && n < NETPLAY_NAME_LEN - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        out[n++] = isalnum(c) || c == '-' ? (char)c : '_';
    }
    if (!n) out[n++] = '?';
    out[n] = 0;
}

/* A profile name (or any text) as one word: hex, "-" when empty. */
static void hex_word(char *out, size_t size, const char *in)
{
    size_t n = 0;
    if (!in || !*in) { snprintf(out, size, "-"); return; }
    for (; *in && n + 3 <= size; in++, n += 2) snprintf(out + n, 3, "%02x", (unsigned char)*in);
    out[n] = 0;
}

static void unhex_word(char *out, size_t size, const char *in)
{
    size_t n = 0;
    unsigned v;
    if (strcmp(in, "-"))
        for (; in[0] && in[1] && n + 1 < size && sscanf(in, "%2x", &v) == 1; in += 2) out[n++] = (char)v;
    out[n] = 0;
}

/* ---- Lines over the sockets ---------------------------------------------------- */
static int send_text(sock_t sock, const char *line, int n)
{
    int ok;
    if (sock == SOCK_BAD) return 0;
    send_lock();
    ok = send(sock, line, n, 0) == n;
    send_unlock();
    return ok;
}

static int format_line(char *line, size_t size, const char *format, va_list args)
{
    int n = vsnprintf(line, size - 1, format, args);
    if (n < 0 || n > (int)size - 2) return -1;
    line[n++] = '\n';
    return n;
}

static int send_to(int p, const char *format, ...)
{
    char line[512];
    va_list args;
    int n;
    va_start(args, format);
    n = format_line(line, sizeof line, format, args);
    va_end(args);
    return n > 0 && send_text(s_peer[p], line, n);
}

/* The host: to every joiner but `except` (-1 for all). A joiner: to the host. */
static void send_all(int except, const char *format, ...)
{
    char line[512];
    va_list args;
    int n, p;
    va_start(args, format);
    n = format_line(line, sizeof line, format, args);
    va_end(args);
    if (n <= 0) return;
    for (p = 0; p < MAXP; p++)
        if (p != except && s_peer[p] != SOCK_BAD) send_text(s_peer[p], line, n);
}

/* A whole line already in the buffer of connection `p`: 1, else 0. */
static int take_line(int p, char *out, size_t size)
{
    char *nl = memchr(s_in[p], '\n', s_in_len[p]);
    size_t len;
    if (!nl) return 0;
    len = (size_t)(nl - s_in[p]);
    if (len >= size) len = size - 1;
    memcpy(out, s_in[p], len);
    out[len] = 0;
    if (len && out[len - 1] == '\r') out[len - 1] = 0;
    s_in_len[p] -= (size_t)(nl - s_in[p]) + 1;
    memmove(s_in[p], nl + 1, s_in_len[p]);
    return 1;
}

/* Read what connection `p` has into its buffer: 0 when it is gone. */
static int fill(int p)
{
    int n;
    /* Full: of whole lines, to be taken first; of a line too long, not our peer. */
    if (s_in_len[p] == sizeof s_in[p]) return memchr(s_in[p], '\n', s_in_len[p]) != NULL;
    n = recv(s_peer[p], s_in[p] + s_in_len[p], (int)(sizeof s_in[p] - s_in_len[p]), 0);
    if (n <= 0) return 0;
    s_in_len[p] += (size_t)n;
    return 1;
}

/* Wait up to `wait_ms` for the listening socket or any connection; marks the ready
 * connections in `ready` and returns whether the listener has a caller. */
static int wait_sockets(sock_t listener, int ready[MAXP], int wait_ms)
{
    fd_set read_set;
    struct timeval tv;
    int p, top = 0, n;
    FD_ZERO(&read_set);
    if (listener != SOCK_BAD) { FD_SET(listener, &read_set); top = (int)listener; }
    for (p = 0; p < MAXP; p++) {
        ready[p] = 0;
        if (s_peer[p] == SOCK_BAD) continue;
        if (memchr(s_in[p], '\n', s_in_len[p])) { ready[p] = 1; wait_ms = 0; }
        FD_SET(s_peer[p], &read_set);
        if ((int)s_peer[p] > top) top = (int)s_peer[p];
    }
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    n = select(top + 1, &read_set, NULL, NULL, &tv);
    if (n <= 0) return 0;
    for (p = 0; p < MAXP; p++)
        if (s_peer[p] != SOCK_BAD && FD_ISSET(s_peer[p], &read_set) && !fill(p)) ready[p] = -1;
        else if (s_peer[p] != SOCK_BAD && FD_ISSET(s_peer[p], &read_set)) ready[p] = 1;
    return listener != SOCK_BAD && FD_ISSET(listener, &read_set);
}

/* The next line from connection `p` within `wait_ms`: 1, 0 for none, -1 when gone. */
static int recv_line(int p, char *out, size_t size, int wait_ms)
{
    int ready[MAXP], waited = 0;
    for (;;) {
        if (take_line(p, out, size)) return 1;
        if (waited >= wait_ms || s_cancel) return 0;
        wait_sockets(SOCK_BAD, ready, 100);
        if (ready[p] < 0) return -1;
        waited += 100;
    }
}

static void drop(int p)
{
    if (s_peer[p] != SOCK_BAD) sock_close(s_peer[p]);
    s_peer[p] = SOCK_BAD;
    s_in_len[p] = 0;
}

static void no_delay(sock_t sock)
{
    int yes = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);
}

/* ---- The lobby ---------------------------------------------------------------------- */
static void default_lobby(void)
{
    int i;
    memset(&s_lobby, 0, sizeof s_lobby);
    s_lobby.local_player = -1;
    s_lobby.match.venue = DEFAULT_VENUE;
    s_lobby.match.slots = 2;
    for (i = 0; i < MAXP; i++) {
        s_lobby.match.fighter[i] = s_default_fighter[i];
        s_lobby.match.team[i] = i;
        s_lobby.match.owner[i] = -1;
    }
}

/* Under the lock: the slots the match type has, their teams, and who plays which. The
 * players fill the slots in order, the host first; the rest are the CPU. */
static void settle_match(NetplayMatch *m, const NetplayLobby *l)
{
    const NetplayMatchType *t = netplay_match_type(m->type);
    int p, slot = 0, i;
    m->slots = t ? t->fighters : 2;
    if (t && t->venue >= 0) m->venue = t->venue;
    if (t && !t->rules) m->rules = 0;
    for (i = 0; i < MAXP; i++) {
        m->owner[i] = -1;
        /* Team Match: slots 0 and 1 against 2 and 3; otherwise everyone for themselves. */
        m->team[i] = m->type == 1 ? i / 2 : i;
    }
    for (p = 0; p < MAXP; p++)
        if (l->name[p][0] && slot < MAXP) m->owner[slot++] = p;
    /* A created fighter is its player's: a slot the CPU now has gets a fighter again. */
    for (i = 0; i < MAXP; i++) {
        if (m->fighter[i] == NETPLAY_CREATED_FIGHTER && m->owner[i] >= 0 && m->profile[i][0]) continue;
        m->profile[i][0] = 0;
        if (m->fighter[i] == NETPLAY_CREATED_FIGHTER) {
            int f = 0, k, taken;
            do {
                for (taken = 0, k = 0; k < MAXP; k++) taken |= k != i && m->fighter[k] == s_default_fighter[f];
            } while (taken && ++f < MAXP);
            m->fighter[i] = s_default_fighter[f < MAXP ? f : i];
        }
    }
}

/* " <profile0> ... <profile3>", in hex */
static void profiles_text(char *out, size_t size, const NetplayMatch *m)
{
    char word[2 * NETPLAY_PROFILE_LEN + 1];
    size_t n = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < MAXP && n < size; i++) {
        hex_word(word, sizeof word, m->profile[i]);
        n += (size_t)snprintf(out + n, size - n, " %s", word);
    }
}

static void read_profiles(const char *text, NetplayMatch *m)
{
    char words[MAXP][2 * NETPLAY_PROFILE_LEN + 1];
    int i, got = sscanf(text, "%64s %64s %64s %64s", words[0], words[1], words[2], words[3]);
    for (i = 0; i < MAXP; i++) {
        if (i < got) unhex_word(m->profile[i], sizeof m->profile[i], words[i]);
        else m->profile[i][0] = 0;
    }
}

static void lobby_line(char *out, size_t size)
{
    const NetplayMatch *m = &s_lobby.match;
    char names[MAXP][NETPLAY_NAME_LEN], profiles[4 * (2 * NETPLAY_PROFILE_LEN + 1) + 1];
    int i;
    for (i = 0; i < MAXP; i++) snprintf(names[i], sizeof names[i], "%s", s_lobby.name[i][0] ? s_lobby.name[i] : "-");
    profiles_text(profiles, sizeof profiles, m);
    int ready = 0;
    for (i = 0; i < MAXP; i++) ready |= (s_lobby.ready[i] != 0) << i;
    snprintf(out, size, "LOBBY %d %d %d %d %d %d %d %d %d %d %d %s %s %s %s%s", m->type, m->rules, m->venue,
             m->fighter[0], m->fighter[1], m->fighter[2], m->fighter[3], m->slots, s_lobby.players,
             s_lobby.phase, ready, names[0], names[1], names[2], names[3], profiles);
}

static int read_lobby_line(const char *line, NetplayLobby *l)
{
    NetplayMatch *m = &l->match;
    char names[MAXP][NETPLAY_NAME_LEN];
    int i;
    int used = 0, ready = 0;
    if (sscanf(line, "LOBBY %d %d %d %d %d %d %d %d %d %d %d %15s %15s %15s %15s%n", &m->type, &m->rules,
               &m->venue, &m->fighter[0], &m->fighter[1], &m->fighter[2], &m->fighter[3], &m->slots, &l->players,
               &l->phase, &ready, names[0], names[1], names[2], names[3], &used) != 15)
        return 0;
    for (i = 0; i < MAXP; i++) l->ready[i] = ready >> i & 1;
    read_profiles(line + used, m);
    for (i = 0; i < MAXP; i++) snprintf(l->name[i], sizeof l->name[i], "%s", strcmp(names[i], "-") ? names[i] : "");
    settle_match(m, l);     /* who has which slot follows from the names, as on the host */
    return 1;
}

static void match_line(char *out, size_t size, const NetplayMatch *m)
{
    char profiles[4 * (2 * NETPLAY_PROFILE_LEN + 1) + 1];
    profiles_text(profiles, sizeof profiles, m);
    snprintf(out, size, "MATCH %u %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d%s", m->seed, m->type, m->rules,
             m->venue, m->slots, m->fighter[0], m->fighter[1], m->fighter[2], m->fighter[3], m->team[0],
             m->team[1], m->team[2], m->team[3], m->owner[0], m->owner[1], m->owner[2], m->owner[3], profiles);
}

static int read_match_line(const char *line, NetplayMatch *m)
{
    int used = 0;
    if (sscanf(line, "MATCH %u %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d%n", &m->seed, &m->type, &m->rules,
               &m->venue, &m->slots, &m->fighter[0], &m->fighter[1], &m->fighter[2], &m->fighter[3],
               &m->team[0], &m->team[1], &m->team[2], &m->team[3], &m->owner[0], &m->owner[1],
               &m->owner[2], &m->owner[3], &used) != 17)
        return 0;
    read_profiles(line + used, m);
    return 1;
}

/* A fighter can be in a match once: is `fighter` taken by a slot other than `slot`?
 * Created fighters are the profiles': one profile can be in a match once. */
static int fighter_taken(const NetplayMatch *m, int slot, int fighter, const char *profile)
{
    int i;
    for (i = 0; i < m->slots; i++) {
        if (i == slot || m->fighter[i] != fighter) continue;
        if (fighter != NETPLAY_CREATED_FIGHTER || !strcmp(m->profile[i], profile ? profile : "")) return 1;
    }
    return 0;
}

/* Under the lock: `slot` gets `fighter` (and the profile of a created one) if it is free. */
static void set_fighter(NetplayMatch *m, int slot, int fighter, const char *profile)
{
    if (fighter == NETPLAY_CREATED_FIGHTER && (!profile || !*profile)) return;
    if (fighter_taken(m, slot, fighter, profile)) return;
    m->fighter[slot] = fighter;
    snprintf(m->profile[slot], sizeof m->profile[slot], "%s",
             fighter == NETPLAY_CREATED_FIGHTER ? profile : "");
}

static int slot_of(const NetplayMatch *m, int player)
{
    int i;
    for (i = 0; i < MAXP; i++)
        if (m->owner[i] == player) return i;
    return -1;
}

/* The host: a player's fighter goes to the slot that player would have. */
static void host_pick(int player, int fighter, const char *profile)
{
    NetplayMatch m;
    int slot;
    lock();
    m = s_lobby.match;
    settle_match(&m, &s_lobby);
    slot = slot_of(&m, player);
    if (slot >= 0 && fighter > 0) set_fighter(&s_lobby.match, slot, fighter, profile);
    s_lobby_dirty = 1;     /* a refused pick goes back to the player as it was */
    unlock();
}

void netplay_set_name(const char *name)
{
    clean_name(s_name, name);
}

void netplay_set_match(int type, int rules, int venue)
{
    if (s_inited) lock();
    if (netplay_match_type(type)) s_lobby.match.type = type;
    if (rules >= 0 && rules <= 2) s_lobby.match.rules = rules;
    if (venue >= 0) s_lobby.match.venue = venue;
    settle_match(&s_lobby.match, &s_lobby);
    s_lobby_dirty = 1;
    if (s_inited) unlock();
}

void netplay_set_slot(int slot, int fighter, int team)
{
    (void)team;     /* teams follow the match type */
    /* The CPU's slots: a created fighter is a player's own. */
    if (slot < 0 || slot >= MAXP || fighter <= 0 || fighter == NETPLAY_CREATED_FIGHTER) return;
    if (s_inited) lock();
    set_fighter(&s_lobby.match, slot, fighter, NULL);
    s_lobby_dirty = 1;
    if (s_inited) unlock();
}

static void pick(int fighter, const char *profile)
{
    if (s_inited) lock();
    if (s_hosting) {
        set_fighter(&s_lobby.match, 0, fighter, profile);
        s_lobby_dirty = 1;
    } else {
        int slot = slot_of(&s_lobby.match, s_lobby.local_player);
        if (slot >= 0) set_fighter(&s_lobby.match, slot, fighter, profile);
        s_pick_dirty = fighter;
        snprintf(s_pick_profile, sizeof s_pick_profile, "%s", profile ? profile : "");
    }
    if (s_inited) unlock();
}

void netplay_pick(int fighter)
{
    if (fighter > 0 && fighter != NETPLAY_CREATED_FIGHTER) pick(fighter, NULL);
}

void netplay_pick_created(const char *profile)
{
    if (profile && *profile) pick(NETPLAY_CREATED_FIGHTER, profile);
}

int netplay_start(void)
{
    const NetplayMatchType *t;
    int players, ok = 1;
    if (!s_hosting || netplay_state() != NETPLAY_LISTENING) return 0;
    lock();
    t = netplay_match_type(s_lobby.match.type);
    players = s_lobby.players;
    unlock();
    if (players < 2) { set_state(NETPLAY_LISTENING, "Nobody has joined yet"); ok = 0; }
    else if (t && players > t->fighters) {
        set_state(NETPLAY_LISTENING, "%s is for %d fighters and %d players are here", t->name, t->fighters, players);
        ok = 0;
    } else {
        int p, waiting = 0;
        lock();
        for (p = 0; p < MAXP; p++) waiting += s_lobby.name[p][0] && !s_lobby.ready[p];
        if (s_lobby.phase != NETPLAY_PHASE_PICKING) waiting = -1;
        unlock();
        if (waiting < 0) { set_state(NETPLAY_LISTENING, "Pick the fighters first"); ok = 0; }
        else if (waiting) { set_state(NETPLAY_LISTENING, "%d player(s) have not locked in a fighter", waiting); ok = 0; }
    }
    if (ok) { lock(); s_start_request = 1; unlock(); }
    return ok;
}

void netplay_set_ready(int ready)
{
    if (s_inited) lock();
    if (s_hosting) {
        s_lobby.ready[0] = ready != 0;
        s_lobby_dirty = 1;
    } else {
        if (s_lobby.local_player >= 0) s_lobby.ready[s_lobby.local_player] = ready != 0;
        s_ready_value = ready != 0;
        s_ready_dirty = 1;
    }
    if (s_inited) unlock();
}

void netplay_set_phase(int phase)
{
    int p;
    if (!s_hosting || (phase != NETPLAY_PHASE_WAITING && phase != NETPLAY_PHASE_PICKING)) return;
    if (s_inited) lock();
    s_lobby.phase = phase;
    for (p = 0; p < MAXP; p++) s_lobby.ready[p] = 0;
    s_lobby_dirty = 1;
    if (s_inited) unlock();
}

int netplay_local_addresses(char out[][48], int max)
{
    char host[256];
    struct addrinfo hints, *found = NULL, *a;
    int n = 0;
    if (!s_inited || gethostname(host, sizeof host) != 0) return 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &found) != 0) return 0;
    for (a = found; a && n < max; a = a->ai_next) {
        const unsigned char *ip = (const unsigned char *)&((struct sockaddr_in *)a->ai_addr)->sin_addr;
        if (ip[0] == 127) continue;
        snprintf(out[n++], 48, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    freeaddrinfo(found);
    return n;
}

void netplay_choose(int venue, int fighter0, int fighter1)
{
    if (s_inited) lock();
    if (venue > 0) s_lobby.match.venue = venue;
    if (fighter0 > 0) s_lobby.match.fighter[0] = fighter0;
    if (fighter1 > 0) s_lobby.match.fighter[1] = fighter1;
    if (s_inited) unlock();
}

/* ---- Connecting ----------------------------------------------------------------- */
static sock_t open_listener(uint16_t port)
{
    struct sockaddr_in addr;
    sock_t server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    if (server == SOCK_BAD) return SOCK_BAD;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(server, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(server, 4) != 0) {
        sock_close(server);
        return SOCK_BAD;
    }
    return server;
}

static sock_t connect_to(const char *address)
{
    char host[128];
    const char *colon = strrchr(address, ':');
    const char *port = colon ? colon + 1 : NULL;
    char port_text[8];
    struct addrinfo hints, *found = NULL, *a;
    sock_t sock = SOCK_BAD;
    size_t len = colon ? (size_t)(colon - address) : strlen(address);
    if (len >= sizeof host) len = sizeof host - 1;
    memcpy(host, address, len);
    host[len] = 0;
    if (!port || !*port) {
        snprintf(port_text, sizeof port_text, "%d", NETPLAY_DEFAULT_PORT);
        port = port_text;
    }
    set_state(NETPLAY_CONNECTING, "Connecting to %s:%s", host, port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &found) != 0) {
        set_state(NETPLAY_FAILED, "Could not find %s", host);
        return SOCK_BAD;
    }
    /* The host may still be starting: try for a while. */
    while (!s_cancel && sock == SOCK_BAD) {
        for (a = found; a && sock == SOCK_BAD; a = a->ai_next) {
            sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (sock != SOCK_BAD && connect(sock, a->ai_addr, (int)a->ai_addrlen) != 0) {
                sock_close(sock);
                sock = SOCK_BAD;
            }
        }
        if (sock == SOCK_BAD) sleep_ms(500);
    }
    freeaddrinfo(found);
    return sock;
}

/* Why a HELLO cannot join, or NULL when it can. */
static const char *hello_problem(const char *line, char *name)
{
    char version[64], program[24], data[24], raw[64];
    int protocol;
    if (sscanf(line, "HELLO %d %63s %23s %23s %63s", &protocol, version, program, data, raw) != 5)
        return "not_Def_Jam_netplay";
    if (protocol != PROTOCOL || strcmp(version, DEFJAM_VERSION)) return "different_versions";
    if (strcmp(program, s_fp_program)) return "different_builds";
    if (strcmp(data, s_fp_data)) return "different_game_data";
    clean_name(name, raw);
    return NULL;
}

/* The host: a caller at the listening socket. */
static void host_accept(sock_t listener)
{
    char line[256], name[NETPLAY_NAME_LEN];
    const char *problem;
    sock_t sock = accept(listener, NULL, NULL);
    int p, slot = -1;
    if (sock == SOCK_BAD) return;
    no_delay(sock);
    for (p = 1; p < MAXP; p++)
        if (s_peer[p] == SOCK_BAD) { slot = p; break; }
    if (slot < 0) {
        send(sock, "REFUSE lobby_full\n", 18, 0);
        sock_close(sock);
        return;
    }
    s_peer[slot] = sock;
    s_in_len[slot] = 0;
    if (recv_line(slot, line, sizeof line, 5000) != 1) { drop(slot); return; }
    problem = hello_problem(line, name);
    if (problem) {
        fprintf(stderr, "[NETPLAY] refused a player: %s\n", problem);
        fflush(stderr);
        send_to(slot, "REFUSE %s", problem);
        drop(slot);
        return;
    }
    send_to(slot, "WELCOME %d", slot);
    lock();
    snprintf(s_lobby.name[slot], sizeof s_lobby.name[slot], "%s", name);
    s_lobby.ready[slot] = 0;
    s_lobby.players++;
    settle_match(&s_lobby.match, &s_lobby);
    s_lobby_dirty = 1;
    unlock();
    set_state(NETPLAY_LISTENING, "%s joined (%d players)", name, s_lobby.players);
}

static void host_lost(int p)
{
    char name[NETPLAY_NAME_LEN];
    drop(p);
    lock();
    snprintf(name, sizeof name, "%s", s_lobby.name[p]);
    s_lobby.name[p][0] = 0;
    s_lobby.ready[p] = 0;
    s_lobby.players--;
    settle_match(&s_lobby.match, &s_lobby);
    s_lobby_dirty = 1;
    unlock();
    set_state(NETPLAY_LISTENING, "%s left (%d players)", name, s_lobby.players);
}

/* The host's lobby: joiners come and go, settings change, until the host starts. */
static int host_lobby(void)
{
    sock_t listener = open_listener(s_port);
    char line[512];
    int ready[MAXP], p;
    if (listener == SOCK_BAD) {
        set_state(NETPLAY_FAILED, "Could not listen on port %u (is another program using it?)", s_port);
        return 0;
    }
    /* Players over the internet need the port forwarded to this PC: ask the router to
     * (netplay_router.c), in the background; the lobby shows how that went. */
    netplay_router_open(s_port);
    lock();
    snprintf(s_lobby.name[0], sizeof s_lobby.name[0], "%s", s_name);
    s_lobby.players = 1;
    s_lobby.local_player = 0;
    settle_match(&s_lobby.match, &s_lobby);
    unlock();
    set_state(NETPLAY_LISTENING, "Hosting on port %u: waiting for players", s_port);
    for (;;) {
        int start;
        if (s_cancel) { sock_close(listener); return 0; }
        if (wait_sockets(listener, ready, 100)) host_accept(listener);
        for (p = 1; p < MAXP; p++) {
            if (ready[p] < 0) { host_lost(p); continue; }
            while (s_peer[p] != SOCK_BAD && take_line(p, line, sizeof line)) {
                char word[2 * NETPLAY_PROFILE_LEN + 1] = "-", profile[NETPLAY_PROFILE_LEN];
                int fighter;
                int ready;
                if (sscanf(line, "PICK %d %64s", &fighter, word) >= 1) {
                    unhex_word(profile, sizeof profile, word);
                    host_pick(p, fighter, profile);
                } else if (sscanf(line, "READYUP %d", &ready) == 1) {
                    lock();
                    s_lobby.ready[p] = ready != 0;
                    s_lobby_dirty = 1;
                    unlock();
                }
            }
        }
        lock();
        if (s_lobby_dirty) {
            lobby_line(line, sizeof line);
            s_lobby_dirty = 0;
            unlock();
            send_all(-1, "%s", line);
        } else {
            unlock();
        }
        lock();
        /* A scripted host starts once enough players are in and every joiner has said
         * it is ready (a scripted joiner does once it has sent its pick). */
        {
            int all_ready = 1;
            for (p = 1; p < MAXP; p++) all_ready &= !s_lobby.name[p][0] || s_lobby.ready[p];
            start = s_start_request || (s_wait_players > 0 && getenv("RECOMP_NETPLAY_HOST")
                                        && s_lobby.players >= s_wait_players && all_ready);
        }
        s_start_request = 0;
        unlock();
        if (start) break;
    }
    sock_close(listener);
    return 1;
}

/* ---- The match's saves ------------------------------------------------------------ *
 * A created fighter is part of its profile's save, so every created fighter's profile
 * goes to every game. Each game plays from its player's own saves -- their profiles,
 * their options and unlocks, and what the game saves during the session stays theirs --
 * with the other players' profiles added for the session (place_saves). */
#define MAX_FILES       24
#define MAX_FILE        (512 * 1024)
#define CHUNK           200         /* bytes per DATA line */

typedef struct {
    char folder[64], name[64];      /* UserData/45410049/<folder>/<name> */
    unsigned char *data;
    size_t size, got;
} SaveFile;

static SaveFile s_files[MAX_FILES];
static int s_file_count;
static int s_rx_file[MAXP] = {-1, -1, -1, -1};     /* the file each connection is sending */
static char s_session_dir[1024];

static void free_files(void)
{
    int i;
    for (i = 0; i < s_file_count; i++) free(s_files[i].data);
    s_file_count = 0;
    for (i = 0; i < MAXP; i++) s_rx_file[i] = -1;
}

/* A folder or file name from the other side: a name, not a path. */
static int plain_name(const char *name)
{
    return name[0] && strcmp(name, ".") && strcmp(name, "..") && !strpbrk(name, "/\\:*?\"<>|");
}

static SaveFile *add_file(const char *folder, const char *name, size_t size)
{
    SaveFile *f = NULL;
    int i;
    if (size > MAX_FILE || !plain_name(folder) || !plain_name(name)) return NULL;
    for (i = 0; i < s_file_count && !f; i++)
        if (!strcmp(s_files[i].folder, folder) && !strcmp(s_files[i].name, name)) f = &s_files[i];
    if (!f) {
        if (s_file_count == MAX_FILES) return NULL;
        f = &s_files[s_file_count++];
        f->data = NULL;
    }
    free(f->data);
    f->data = (unsigned char *)malloc(size ? size : 1);
    if (!f->data) { s_file_count--; return NULL; }
    snprintf(f->folder, sizeof f->folder, "%s", folder);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->size = size;
    f->got = 0;
    return f;
}

/* Every file of the save `name` (a profile, or "Options") in this player's saves. */
static int add_own_save(const char *name)
{
    char root[1024], folder[64], dir[1200], path[1300], files[16][64];
    int n, i, ok = 0;
    if (!netplay_save_root(root, sizeof root) || !netplay_save_folder(root, name, folder)) return 0;
    snprintf(dir, sizeof dir, "%s" PATH_SEP "UserData" PATH_SEP "45410049" PATH_SEP "%s", root, folder);
    n = netplay_list_dir(dir, 0, files, 16);
    for (i = 0; i < n; i++) {
        FILE *in;
        long size;
        SaveFile *f;
        snprintf(path, sizeof path, "%s" PATH_SEP "%s", dir, files[i]);
        if (!(in = fopen(path, "rb"))) continue;
        fseek(in, 0, SEEK_END);
        size = ftell(in);
        fseek(in, 0, SEEK_SET);
        f = size >= 0 ? add_file(folder, files[i], (size_t)size) : NULL;
        if (f) f->got = fread(f->data, 1, f->size, in);
        fclose(in);
        ok |= f && f->got == f->size && !strcmp(files[i], name);
    }
    return ok;
}

static int have_save(const char *name)
{
    int i;
    for (i = 0; i < s_file_count; i++)
        if (!strcmp(s_files[i].name, name) && s_files[i].got == s_files[i].size) return 1;
    return 0;
}

static int send_file(int p, const SaveFile *f)
{
    char word[2 * sizeof f->name + 1], hex[2 * CHUNK + 1];
    size_t off, i, n;
    hex_word(word, sizeof word, f->name);
    if (!send_to(p, "FILE %s %s %u", f->folder, word, (unsigned)f->size)) return 0;
    for (off = 0; off < f->size; off += n) {
        n = f->size - off < CHUNK ? f->size - off : CHUNK;
        for (i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", f->data[off + i]);
        if (!send_to(p, "DATA %s", hex)) return 0;
    }
    return send_to(p, "END");
}

static int send_files(int p)
{
    int i;
    for (i = 0; i < s_file_count; i++)
        if (s_files[i].got == s_files[i].size && !send_file(p, &s_files[i])) return 0;
    return 1;
}

/* A FILE, DATA or END line from connection `p`: taken (1), or not one (0). */
static int file_line(int p, const char *line)
{
    char folder[64], word[130], name[64];
    unsigned size;
    if (sscanf(line, "FILE %63s %129s %u", folder, word, &size) == 3) {
        SaveFile *f;
        unhex_word(name, sizeof name, word);
        f = add_file(folder, name, size);
        s_rx_file[p] = f ? (int)(f - s_files) : -1;
        return 1;
    }
    if (!strncmp(line, "DATA ", 5)) {
        SaveFile *f = s_rx_file[p] >= 0 ? &s_files[s_rx_file[p]] : NULL;
        const char *hex = line + 5;
        unsigned v;
        for (; f && hex[0] && hex[1] && f->got < f->size && sscanf(hex, "%2x", &v) == 1; hex += 2)
            f->data[f->got++] = (unsigned char)v;
        return 1;
    }
    if (!strcmp(line, "END")) { s_rx_file[p] = -1; return 1; }
    return 0;
}

/* The session's save folder, next to the player's own saves, made afresh from s_files:
 * the fallback of place_saves. */
static int write_session(void)
{
    char root[1024], dir[1200], path[1400], folders[64][64], files[16][64];
    int n, i, k, j;
    if (!netplay_save_root(root, sizeof root)) return 0;
    /* One per player, so two games on one computer (a test) keep apart. */
    snprintf(s_session_dir, sizeof s_session_dir, "%s" PATH_SEP "netplay-session-%d", root, s_lobby.local_player + 1);
    snprintf(dir, sizeof dir, "%s" PATH_SEP "UserData" PATH_SEP "45410049", s_session_dir);
    n = netplay_list_dir(dir, 1, folders, 64);
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s" PATH_SEP "%s", dir, folders[i]);
        k = netplay_list_dir(path, 0, files, 16);
        for (j = 0; j < k; j++) {
            snprintf(path, sizeof path, "%s" PATH_SEP "%s" PATH_SEP "%s", dir, folders[i], files[j]);
            remove(path);
        }
        snprintf(path, sizeof path, "%s" PATH_SEP "%s", dir, folders[i]);
        remove_dir(path);
    }
    make_dir(root);
    make_dir(s_session_dir);
    snprintf(path, sizeof path, "%s" PATH_SEP "UserData", s_session_dir);
    make_dir(path);
    make_dir(dir);
    for (i = 0; i < s_file_count; i++) {
        const SaveFile *f = &s_files[i];
        FILE *out;
        if (f->got != f->size) continue;
        snprintf(path, sizeof path, "%s" PATH_SEP "%s", dir, f->folder);
        make_dir(path);
        snprintf(path, sizeof path, "%s" PATH_SEP "%s" PATH_SEP "%s", dir, f->folder, f->name);
        if (!(out = fopen(path, "wb"))) return 0;
        if (fwrite(f->data, 1, f->size, out) != f->size) { fclose(out); return 0; }
        fclose(out);
    }
    return 1;
}

/* Is the file at `path` exactly f's contents? */
static int same_file(const char *path, const SaveFile *f)
{
    FILE *in = fopen(path, "rb");
    unsigned char buf[4096];
    size_t off = 0, n;
    int same = 1;
    if (!in) return 0;
    while (same && (n = fread(buf, 1, sizeof buf, in)) > 0) {
        same = off + n <= f->size && !memcmp(buf, f->data + off, n);
        off += n;
    }
    fclose(in);
    return same && off == f->size;
}

static int dir_exists(const char *path)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void remove_folder(const char *dir)
{
    char files[16][64], path[1400];
    int n = netplay_list_dir(dir, 0, files, 16), i;
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s" PATH_SEP "%s", dir, files[i]);
        remove(path);
    }
    remove_dir(dir);
}

/* The profiles added to the player's saves for a session are listed in
 * <save root>/netplay-guests.txt, one folder a line, and taken out again when the game
 * closes, or, after a crash, at the next start (netplay_startup). Only folders this
 * code made are ever listed. */
void netplay_remove_guests(void)
{
    char root[1024], list[1100], line[128], dir[1300];
    FILE *in;
    if (!netplay_save_root(root, sizeof root)) return;
    snprintf(list, sizeof list, "%s" PATH_SEP "netplay-guests.txt", root);
    if (!(in = fopen(list, "r"))) return;
    while (fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!plain_name(line)) continue;
        snprintf(dir, sizeof dir, "%s" PATH_SEP "UserData" PATH_SEP "45410049" PATH_SEP "%s", root, line);
        remove_folder(dir);
        fprintf(stderr, "[NETPLAY] removed the session's guest profile %s\n", line);
    }
    fclose(in);
    remove(list);
    fflush(stderr);
}

static char s_user_dir[1100];           /* UserData/45410049 the match plays from */

/* Where this game plays the match from: the player's own saves, with every created
 * fighter's profile they do not have added for the session. A profile folder already
 * there with other contents is a save of the player's own by the same name; then, so
 * as to touch neither, this game plays from a session folder of its own instead
 * (RECOMP_SAVE_DIR: the match's profiles and the player's own options). */
static int place_saves(void)
{
    char root[1024], dir[1300], path[1400], list[1100];
    int i, j, clash = 0, added = 0;
    FILE *guests;
    s_session_dir[0] = 0;
    if (!netplay_save_root(root, sizeof root)) return 0;
    netplay_remove_guests();
    snprintf(s_user_dir, sizeof s_user_dir, "%s" PATH_SEP "UserData" PATH_SEP "45410049", root);
    for (i = 0; i < s_file_count && !clash; i++) {
        const SaveFile *f = &s_files[i];
        snprintf(path, sizeof path, "%s" PATH_SEP "%s", s_user_dir, f->folder);
        if (!dir_exists(path)) continue;
        snprintf(path, sizeof path, "%s" PATH_SEP "%s" PATH_SEP "%s", s_user_dir, f->folder, f->name);
        if (f->got == f->size && !same_file(path, f)) clash = 1;
    }
    if (clash) {
        fprintf(stderr, "[NETPLAY] a profile of the match has the name of one of this player's own; "
                        "playing from a session save folder\n");
        fflush(stderr);
        add_own_save("Options");
        if (!write_session()) return 0;
        snprintf(s_user_dir, sizeof s_user_dir, "%s" PATH_SEP "UserData" PATH_SEP "45410049", s_session_dir);
        return 1;
    }
    snprintf(list, sizeof list, "%s" PATH_SEP "netplay-guests.txt", root);
    for (i = 0; i < s_file_count; i++) {
        const SaveFile *f = &s_files[i];
        FILE *out;
        int seen = 0;
        if (f->got != f->size) continue;
        snprintf(dir, sizeof dir, "%s" PATH_SEP "%s", s_user_dir, f->folder);
        if (dir_exists(dir)) {
            for (j = 0; j < i; j++) seen |= !strcmp(s_files[j].folder, f->folder);
            if (!seen) continue;            /* the player's own (the same contents) */
        } else {
            make_dir(root);
            snprintf(path, sizeof path, "%s" PATH_SEP "UserData", root);
            make_dir(path);
            make_dir(s_user_dir);
            make_dir(dir);
            if ((guests = fopen(list, "a")) != NULL) {
                fprintf(guests, "%s\n", f->folder);
                fclose(guests);
            }
            added++;
            fprintf(stderr, "[NETPLAY] added %s's profile folder %s to this player's saves for the session\n",
                    f->name, f->folder);
        }
        snprintf(path, sizeof path, "%s" PATH_SEP "%s", dir, f->name);
        if (!(out = fopen(path, "wb"))) return 0;
        if (fwrite(f->data, 1, f->size, out) != f->size) { fclose(out); return 0; }
        fclose(out);
    }
    if (added) atexit(netplay_remove_guests);
    fflush(stderr);
    return 1;
}

static int by_folder(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* The user number of `profile` among the saves the match plays from: the game lists its
 * profiles (not the options) in the order of their folders. -1 when it is not there. */
static int profile_user(const char *profile)
{
    char folders[64][64], files[16][64], path[1300];
    int n = netplay_list_dir(s_user_dir, 1, folders, 64), i, j, k, user = 0;
    for (i = 0; i < n; i++) {
        int is_profile = 0, mine = 0;
        snprintf(path, sizeof path, "%s" PATH_SEP "%s", s_user_dir, folders[i]);
        k = netplay_list_dir(path, 0, files, 16);
        for (j = 0; j < k; j++) {
            if (!netplay_save_file(files[j]) || !strcmp(files[j], "Options")) continue;
            is_profile = 1;
            mine |= !strcmp(files[j], profile);
        }
        if (mine) return user;
        user += is_profile;
    }
    return -1;
}

/* The host: settle the match, send it, gather the saves, and wait for every READY. */
static int host_agree(void)
{
    NetplayMatch m;
    const char *fixed = getenv("RECOMP_NETPLAY_SEED");
    char line[512];
    int p, i, waiting, waited = 0, ready[MAXP];
    lock();
    m = s_lobby.match;
    settle_match(&m, &s_lobby);
    unlock();
    m.seed = fixed && *fixed ? (uint32_t)strtoul(fixed, NULL, 0) : fresh_seed();
    m.local_slot = slot_of(&m, 0);
    free_files();
    for (i = 0; i < m.slots; i++)
        if (m.fighter[i] == NETPLAY_CREATED_FIGHTER && m.owner[i] == 0 && !add_own_save(m.profile[i])) {
            set_state(NETPLAY_FAILED, "Could not read your profile %s", m.profile[i]);
            return 0;
        }
    match_line(line, sizeof line, &m);
    send_all(-1, "%s", line);
    set_state(NETPLAY_HANDSHAKE, "Starting: gathering the created fighters");
    for (waiting = 0, p = 1; p < MAXP; p++) waiting |= (s_peer[p] != SOCK_BAD) << p;
    while (waiting && waited < AGREE_MS && !s_cancel) {
        wait_sockets(SOCK_BAD, ready, 100);
        waited += 100;
        for (p = 1; p < MAXP; p++) {
            if (!(waiting >> p & 1)) continue;
            if (ready[p] < 0) { set_state(NETPLAY_FAILED, "%s left while the match was starting", s_lobby.name[p]); return 0; }
            while (take_line(p, line, sizeof line))
                if (!file_line(p, line) && !strcmp(line, "SENT")) waiting &= ~(1 << p);
        }
    }
    if (waiting) {
        if (!s_cancel) set_state(NETPLAY_FAILED, "Not every player answered");
        return 0;
    }
    for (i = 0; i < m.slots; i++)
        if (m.fighter[i] == NETPLAY_CREATED_FIGHTER && !have_save(m.profile[i])) {
            set_state(NETPLAY_FAILED, "%s's profile %s did not arrive", s_lobby.name[m.owner[i]], m.profile[i]);
            return 0;
        }
    for (p = 1; p < MAXP; p++)
        if (s_peer[p] != SOCK_BAD && !(send_files(p) && send_to(p, "FILES"))) {
            set_state(NETPLAY_FAILED, "%s left while the match was starting", s_lobby.name[p]);
            return 0;
        }
    if (!place_saves()) { set_state(NETPLAY_FAILED, "Could not put the match's profiles in place"); return 0; }
    set_state(NETPLAY_HANDSHAKE, "Starting: waiting for every player to agree");
    for (waiting = 0, p = 1; p < MAXP; p++) waiting |= (s_peer[p] != SOCK_BAD) << p;
    waited = 0;
    while (waiting && waited < AGREE_MS && !s_cancel) {
        wait_sockets(SOCK_BAD, ready, 100);
        waited += 100;
        for (p = 1; p < MAXP; p++) {
            if (!(waiting >> p & 1)) continue;
            if (ready[p] < 0) { set_state(NETPLAY_FAILED, "%s left while the match was starting", s_lobby.name[p]); return 0; }
            while (take_line(p, line, sizeof line))
                if (!strcmp(line, "READY")) waiting &= ~(1 << p);
        }
    }
    if (waiting) {
        if (!s_cancel) set_state(NETPLAY_FAILED, "Not every player answered");
        return 0;
    }
    send_all(-1, "GO");
    lock(); s_match = m; unlock();
    return 1;
}

/* A joiner: HELLO, then the lobby until the host's MATCH and GO. */
static int join_session(void)
{
    char line[512], reason[64], word[2 * NETPLAY_PROFILE_LEN + 1];
    int got, player;
    NetplayMatch m;
    set_state(NETPLAY_HANDSHAKE, "Connected: checking that the games match");
    if (!send_to(0, "HELLO %d %s %s %s %s", PROTOCOL, DEFJAM_VERSION, s_fp_program, s_fp_data, s_name)) return 0;
    while ((got = recv_line(0, line, sizeof line, 200)) == 0)
        if (s_cancel) return 0;
    if (got < 0) { set_state(NETPLAY_FAILED, "The host closed the connection"); return 0; }
    if (sscanf(line, "REFUSE %63s", reason) == 1) {
        static const char *why[][2] = {
            {"lobby_full", "The lobby is full"},
            {"different_versions", "The host has a different version"},
            {"different_builds", "The host has a different build; everyone needs the same one"},
            {"different_game_data", "The game data differs (default.xbe); everyone needs the same dump"},
        };
        size_t i;
        for (i = 0; i < sizeof why / sizeof why[0]; i++)
            if (!strcmp(reason, why[i][0])) { set_state(NETPLAY_FAILED, "%s", why[i][1]); return 0; }
        set_state(NETPLAY_FAILED, "The host refused: %s", reason);
        return 0;
    }
    if (sscanf(line, "WELCOME %d", &player) != 1 || player < 1 || player >= MAXP) {
        set_state(NETPLAY_FAILED, "The other side is not a Def Jam netplay host");
        return 0;
    }
    lock(); s_lobby.local_player = player; unlock();
    set_state(NETPLAY_LOBBY, "In the lobby as player %d: waiting for the host to start", player + 1);
    for (;;) {
        int picked, ready;
        got = recv_line(0, line, sizeof line, 100);
        if (s_cancel) return 0;
        if (got < 0) { set_state(NETPLAY_FAILED, "The host left"); return 0; }
        lock();
        picked = s_pick_dirty;
        s_pick_dirty = 0;
        hex_word(word, sizeof word, s_pick_profile);
        ready = s_ready_dirty ? s_ready_value : -1;
        s_ready_dirty = 0;
        unlock();
        if (picked) send_to(0, "PICK %d %s", picked, word);
        if (ready >= 0) send_to(0, "READYUP %d", ready);
        if (got == 0) continue;
        if (!strncmp(line, "LOBBY ", 6)) {
            NetplayLobby l;
            lock();
            l = s_lobby;
            if (read_lobby_line(line, &l)) s_lobby = l;
            unlock();
        } else if (!strncmp(line, "MATCH ", 6)) {
            memset(&m, 0, sizeof m);
            if (!read_match_line(line, &m)) { set_state(NETPLAY_FAILED, "The host sent a match this game cannot read"); return 0; }
            m.local_slot = slot_of(&m, player);
            break;
        }
    }
    /* This player's created fighter goes to the host; every match's save comes back. */
    set_state(NETPLAY_HANDSHAKE, "Starting: sending and receiving the created fighters");
    free_files();
    if (m.local_slot >= 0 && m.fighter[m.local_slot] == NETPLAY_CREATED_FIGHTER)
        add_own_save(m.profile[m.local_slot]);
    if (!send_files(0) || !send_to(0, "SENT")) { set_state(NETPLAY_FAILED, "The host left"); return 0; }
    free_files();
    for (;;) {
        got = recv_line(0, line, sizeof line, 200);
        if (s_cancel) return 0;
        if (got < 0) { set_state(NETPLAY_FAILED, "The host left"); return 0; }
        if (got == 0 || file_line(0, line)) continue;
        if (!strcmp(line, "FILES")) break;
    }
    if (!place_saves()) { set_state(NETPLAY_FAILED, "Could not put the match's profiles in place"); return 0; }
    send_to(0, "READY");
    while ((got = recv_line(0, line, sizeof line, 200)) == 0)
        if (s_cancel) return 0;
    if (got < 0 || strcmp(line, "GO")) { set_state(NETPLAY_FAILED, "The host left"); return 0; }
    lock(); s_match = m; unlock();
    return 1;
}

/* ---- Inputs and the desync check (the game thread and the worker share these) ----- */
#define RING            512         /* steps of inputs kept; far more than any delay */
#define ENTRY_WORDS     6           /* what sub_001BAA00 reads of a pad-table entry */
#define SYNC_EVERY      60          /* steps between desync checks */
#define SYNC_KEEP       (RING / SYNC_EVERY + 1)
/* Steps as they travel and are kept: the match in the high bits, the step in the low
 * 24 (some 77 hours of fight), so every match's steps are new. */
typedef long long net_step;
#define STEP_BITS       24
#define NET_STEP(match, step) (((net_step)(match) << STEP_BITS) | (net_step)(step))
#define STEP_OF(key)    ((long)((key) & ((1 << STEP_BITS) - 1)))
#define NO_STEP         LLONG_MAX

static uint32_t s_inputs[MAXP][RING][ENTRY_WORDS];     /* per player */
static net_step s_input_step[MAXP][RING];   /* which step each ring entry holds */
static net_step s_last_in[MAXP];            /* the last step each player's input arrived for */
static net_step s_at[MAXP];                 /* the highest step each player has reached */
static net_step s_gone_from[MAXP];          /* from this step on the player has no input */
static uint32_t s_sync_peer[MAXP][SYNC_KEEP], s_sync_mine[SYNC_KEEP];
static net_step s_sync_peer_step[MAXP][SYNC_KEEP], s_sync_mine_step[SYNC_KEEP];
static int s_host_lost;                 /* a joiner: the host's connection is gone */
static int s_net_match;                 /* this process's netplay matches so far */
static int s_vote[MAXP];                /* after match s_net_match: -1 none, 0 quit, 1 rematch */

static void store_input(int p, net_step step, const uint32_t *w)
{
    lock();
    memcpy(s_inputs[p][STEP_OF(step) % RING], w, ENTRY_WORDS * sizeof *w);
    s_input_step[p][STEP_OF(step) % RING] = step;
    if (step > s_last_in[p]) s_last_in[p] = step;
    unlock();
}

/* The host: player `p` has left; from the step after their last input on, every game
 * plays their slot empty. The inputs already passed on precede this on each
 * connection, so every joiner reaches the same step with the same inputs. */
static void host_gone(int p)
{
    net_step from;
    drop(p);
    lock();
    from = s_last_in[p] + 1;
    if (s_gone_from[p] == NO_STEP) s_gone_from[p] = from;
    unlock();
    fprintf(stderr, "[NETPLAY] player %d left; their slot plays on without input from step %ld\n", p + 1,
            STEP_OF(from));
    fflush(stderr);
    send_all(-1, "GONE %d %lld", p, from);
}

static void handle_game_line(int from, const char *line)
{
    net_step step;
    int p, match, vote;
    uint32_t w[ENTRY_WORDS], hash;
    if (sscanf(line, "IN %d %lld %x %x %x %x %x %x", &p, &step, &w[0], &w[1], &w[2], &w[3], &w[4], &w[5]) == 8
        && p >= 0 && p < MAXP && step > 0) {
        if (s_hosting) {
            if (p != from) return;
            send_all(from, "%s", line);
        }
        store_input(p, step, w);
    } else if (sscanf(line, "AT %d %lld", &p, &step) == 2 && p >= 0 && p < MAXP) {
        if (s_hosting) {
            if (p != from) return;
            send_all(from, "%s", line);
        }
        lock();
        if (step > s_at[p]) s_at[p] = step;
        unlock();
    } else if (sscanf(line, "SYNC %d %lld %x", &p, &step, &hash) == 3 && p >= 0 && p < MAXP && step > 0) {
        int i = (int)((STEP_OF(step) / SYNC_EVERY) % SYNC_KEEP);
        lock();
        s_sync_peer[p][i] = hash;
        s_sync_peer_step[p][i] = step;
        unlock();
    } else if (!s_hosting && sscanf(line, "GONE %d %lld", &p, &step) == 2 && p >= 0 && p < MAXP) {
        lock();
        if (s_gone_from[p] == NO_STEP) s_gone_from[p] = step;
        unlock();
        fprintf(stderr, "[NETPLAY] player %d left; their slot plays on without input from step %ld\n", p + 1,
                STEP_OF(step));
        fflush(stderr);
    } else if (sscanf(line, "VOTE %d %d %d", &p, &match, &vote) == 3 && p >= 0 && p < MAXP) {
        if (s_hosting) {
            if (p != from) return;
            send_all(from, "%s", line);
        }
        lock();
        if (match == s_net_match) s_vote[p] = vote != 0;
        unlock();
        fprintf(stderr, "[NETPLAY] player %d chose %s after match %d\n", p + 1, vote ? "rematch" : "quit", match);
        fflush(stderr);
    }
}

/* After the agreement the worker only listens and, on the host, passes messages on. */
static void listen_in_game(void)
{
    char line[512];
    int ready[MAXP], p;
    for (;;) {
        int any = 0;
        wait_sockets(SOCK_BAD, ready, 500);
        for (p = 0; p < MAXP; p++) {
            if (s_peer[p] == SOCK_BAD) continue;
            any = 1;
            if (ready[p] < 0) {
                if (s_hosting) {
                    host_gone(p);
                } else {
                    fprintf(stderr, "[NETPLAY] the host's game disconnected\n");
                    fflush(stderr);
                    drop(p);
                    lock(); s_host_lost = 1; unlock();
                }
                continue;
            }
            while (s_peer[p] != SOCK_BAD && take_line(p, line, sizeof line))
                handle_game_line(p, line);
        }
        if (!any) return;
    }
}

#if defined(_WIN32)
static DWORD WINAPI worker_entry(LPVOID arg)
#else
static void *worker_entry(void *arg)
#endif
{
    int ok, p;
    (void)arg;
    if (s_hosting) {
        ok = host_lobby() && host_agree();
    } else {
        s_peer[0] = connect_to(s_address);
        if (s_peer[0] != SOCK_BAD) no_delay(s_peer[0]);
        ok = s_peer[0] != SOCK_BAD && join_session();
    }
    if (!ok) {
        if (netplay_state() != NETPLAY_FAILED)
            set_state(NETPLAY_FAILED, s_cancel ? "Cancelled" : "The connection was lost");
        for (p = 0; p < MAXP; p++) drop(p);
        if (s_hosting) netplay_router_close();
        return 0;
    }
    set_state(NETPLAY_READY, "Match agreed (seed %u): starting", s_match.seed);
    listen_in_game();
    if (s_hosting) netplay_router_close();     /* every player has gone */
    return 0;
}

static int start_session(void)
{
    int p, i;
    if (!s_inited) {
        lock_init();
        if (!net_init()) return 0;
        s_inited = 1;
    }
    if (netplay_state() != NETPLAY_IDLE && netplay_state() != NETPLAY_FAILED) return 0;
    fingerprints();
    s_cancel = 0;
    s_host_lost = 0;
    s_start_request = s_pick_dirty = s_ready_dirty = 0;
    s_pick_profile[0] = 0;
    s_session_dir[0] = 0;
    s_lobby_dirty = 1;
    lock();
    {
        NetplayMatch keep = s_lobby.match;
        default_lobby();
        /* A host keeps the match it set before (a scripted run's, or the last lobby's);
         * one never set (no slots yet) starts from the defaults. */
        if (s_hosting && keep.slots) s_lobby.match = keep;
    }
    unlock();
    for (p = 0; p < MAXP; p++) {
        s_in_len[p] = 0;
        s_at[p] = -1;
        s_last_in[p] = 0;
        s_gone_from[p] = NO_STEP;
        s_vote[p] = -1;
        for (i = 0; i < RING; i++) s_input_step[p][i] = 0;
    }
    if (!start_thread()) {
        set_state(NETPLAY_FAILED, "Could not start the network thread");
        return 0;
    }
    return 1;
}

int netplay_host(uint16_t port)
{
    s_hosting = 1;
    s_port = port ? port : NETPLAY_DEFAULT_PORT;
    return start_session();
}

int netplay_join(const char *address)
{
    s_hosting = 0;
    snprintf(s_address, sizeof s_address, "%s", address);
    return start_session();
}

void netplay_cancel(void)
{
    s_cancel = 1;
}

/* ---- Starting the game ----------------------------------------------------------- */
static void put_env(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int s_session_live;          /* this process plays a netplay match */

/* The front-end commands that build the match from the main menu, the calls Battle's
 * own screens make for it: Choose Match Type (the type, a special type's own venue,
 * the rules), the user-ID screen (pad 0 bound), the fighters, the venue map. Slot 0
 * is set up as the human on pad 0 and every other slot as the CPU, the same on every
 * game: a human slot needs a pad of its own, and match set-up crashed without one
 * (sub_001BBC10). The fighters' pad words are replaced every step from the exchanged
 * inputs instead, which drives a CPU slot as well. */
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

/* The fighters: ControllerSetup(slot, user, fighter, controller): the CPU's slots have
 * no user; the controller is the slot's own, as the fighter screens give it. Team
 * Match: the Choose Teams screen places the fighters in turn, ControllerSelect(slot,
 * user or -(slot+1) for the CPU, fighter+1, place); places 0 and 1 are team 1, 2 and
 * 3 team 2. */
static int human_slot(const NetplayMatch *m, int i)
{
    return i == 0 || m->fighter[i] == NETPLAY_CREATED_FIGHTER;
}

static void fighter_commands(const NetplayMatch *m, char *out, size_t size, size_t *n)
{
    int i;
    for (i = 0; i < m->slots; i++)
        add(out, size, n, "Controller.ControllerSetup(%d,%d,%d,%d);", i, human_slot(m, i) ? i : -1, m->fighter[i], i);
    if (m->type == 1) {
        int place = 0, team;
        for (team = 0; team < 2; team++)
            for (i = 0; i < m->slots; i++)
                if (m->team[i] == team)
                    add(out, size, n, "Controller.ControllerSelect(%d,%d,%d,%d);", i,
                        human_slot(m, i) ? i : -(i + 1), m->fighter[i] + 1, place++);
    }
}

/* With created fighters the commands follow the user-ID screen and the fighter screen
 * as a player goes through them, with their pauses: the profiles are read (GetUserData),
 * bound to their slots and loaded (ContinueLoad, which opens the fighter screen), and
 * once that screen has had time to build them the fighters are set and the match
 * starts. Without the pauses the match loaded with an empty arena (2026-10-30). */
#define PROFILE_FRAMES  120         /* after the profiles are read */
#define BUILD_FRAMES    300         /* on the fighter screen */

static void match_commands(const NetplayMatch *m, char *out, size_t size)
{
    const NetplayMatchType *t = netplay_match_type(m->type);
    size_t n = 0;
    int i, user[MAXP], created = 0;
    for (i = 0; i < MAXP; i++) {
        user[i] = i < m->slots && m->fighter[i] == NETPLAY_CREATED_FIGHTER ? profile_user(m->profile[i]) : -1;
        created |= user[i] >= 0;
    }
    add(out, size, &n, "getscreeninfo(intmain/mainmenu#1=>Game.StartBattleMode();Game.SetMatchType(%d);", m->type);
    if (t && t->venue >= 0) add(out, size, &n, "Game.ChooseVenue(%d);", m->venue);
    add(out, size, &n, "Game.SetMatchRules(%d);", m->rules);
    if (!created) {
        add(out, size, &n, "Controller.ControllerBind(0);");
        fighter_commands(m, out, size, &n);
        add(out, size, &n, "Game.ChooseVenue(%d);Game.StartGame()", m->venue);
        return;
    }
    /* The user-ID screen binds each player's controller; a created fighter's slot is a
     * player's, with its own controller (netplay_startup plugs in enough). */
    add(out, size, &n, "Controller.ControllerBind(-1);MemCard.ChooseMCDevice();Controller.ControllerBind(0);");
    for (i = 1; i < MAXP; i++)
        if (user[i] >= 0) add(out, size, &n, "Controller.ControllerBind(%d);", i);
    add(out, size, &n, "MemCard.GetNumUsers();");
    for (i = 0; i < MAXP; i++)
        if (user[i] >= 0) add(out, size, &n, "MemCard.GetUserData(%d);", user[i]);
    add(out, size, &n, "|getscreeninfo(intmain/mainmenu#1+%d=>", PROFILE_FRAMES);
    if (user[0] < 0) add(out, size, &n, "MemCard.SetBMCurrentUserIndex(0,-1);");
    for (i = 0; i < MAXP; i++)
        if (user[i] >= 0) add(out, size, &n, "MemCard.SetBMCurrentUserIndex(%d,%d);", i, user[i]);
    add(out, size, &n, "Battle.LoadCreatePlayerData(%d);Game.ContinueLoad(%d)", m->slots, m->slots);
    /* The fighter screen reads each bound profile (GetAttributesBM) and then its list. */
    add(out, size, &n, "|resetfighterlist(#1+%d=>", BUILD_FRAMES);
    fighter_commands(m, out, size, &n);
    /* What the match-loading screen asks before it starts the match. */
    add(out, size, &n, "Game.ResetVenueList(0);Game.ChooseVenue(%d);Game.GetMatchInfo();Battle.SetCPViewportCam(%d,%d);",
        m->venue, m->slots, m->slots == 2 ? 1 : 5);
    for (i = 0; i < MAXP; i++)
        if (user[i] >= 0) add(out, size, &n, "Game.GetCreatePlayerName(%d);", i);
    add(out, size, &n, "Controller.GetCustomControlMap();Game.StartGame()");
}

int netplay_startup(void)
{
    const char *host = getenv("RECOMP_NETPLAY_HOST"), *join = getenv("RECOMP_NETPLAY_JOIN");
    NetplayMatch m;
    char text[2048];
    int i;
    if (s_inited && netplay_state() == NETPLAY_IDLE) return 1;
    if (!s_inited || netplay_state() == NETPLAY_FAILED) {
        const char *venue = getenv("RECOMP_NETPLAY_VENUE"), *fighters = getenv("RECOMP_NETPLAY_FIGHTERS");
        const char *type = getenv("RECOMP_NETPLAY_TYPE"), *players = getenv("RECOMP_NETPLAY_PLAYERS");
        const char *name = getenv("RECOMP_NETPLAY_NAME"), *profile = getenv("RECOMP_NETPLAY_PROFILE");
        int f[MAXP] = {0, 0, 0, 0};
        if (!(host && *host) && !(join && *join)) {
            netplay_remove_guests();    /* a session's guests left by a crash */
            return 1;                   /* no netplay asked for */
        }
        default_lobby();
        if (name && *name) netplay_set_name(name);
        if (fighters) sscanf(fighters, "%d,%d,%d,%d", &f[0], &f[1], &f[2], &f[3]);
        for (i = 0; i < MAXP; i++) if (f[i] > 0) s_lobby.match.fighter[i] = f[i];
        if (venue && *venue) s_lobby.match.venue = atoi(venue);
        if (type && *type && netplay_match_type(atoi(type))) s_lobby.match.type = atoi(type);
        s_wait_players = players && *players ? atoi(players) : 2;
        if (host && *host) netplay_host((uint16_t)atoi(host));
        else netplay_join(join);
        if (profile && *profile) netplay_pick_created(profile);
        if (join && *join) netplay_set_ready(1);    /* after the pick: the host may start */
        while (netplay_state() != NETPLAY_READY && netplay_state() != NETPLAY_FAILED)
            sleep_ms(50);
    }
    if (!netplay_match(&m)) return 0;
    snprintf(text, sizeof text, "%u", m.seed);
    /* Straight to the match: the intro is skipped (skip_intro.c asks netplay_quick_start),
     * and from the title screen Start and A are pressed for the player every second --
     * Start once the title takes it (about 7 s in), A for the memory-card notice -- until
     * the main menu, where the match's commands take over (launcher_title_presses). The
     * player's own pad stays live (RECOMP_PAD_HOST). A test's own pad script is left as
     * it is. */
    if (!getenv("RECOMP_PAD_SCRIPT")) {
        char script[1400];
        launcher_title_presses(script, sizeof script);
        put_env("RECOMP_SCRIPT_ANCHOR", "getscreeninfo(intmain/prestart");
        put_env("RECOMP_PAD_SCRIPT", script);
        put_env("RECOMP_PAD_SCRIPT_FORMAT", "seconds");     /* name:from:to, in seconds */
        put_env("RECOMP_PAD_HOST", "1");
    }
    put_env("RECOMP_LOCKSTEP", "1");
    put_env("RECOMP_LOCKSTEP_SEED", text);
    /* The player's own saves, with the match's profiles added (place_saves); a session
     * folder only when a profile's name clashed with one of the player's. */
    if (s_session_dir[0]) put_env("RECOMP_SAVE_DIR", s_session_dir);
    {
        /* A created fighter's slot is set up as a player's, and a player's slot needs a
         * controller on the hub (match set-up crashed without one, sub_001BBC10); its
         * input still comes from the session. */
        const char *pads = getenv("RECOMP_USB_PADS");
        int need = 1;
        for (i = 1; i < m.slots && i < MAXP; i++)
            if (m.fighter[i] == NETPLAY_CREATED_FIGHTER) need = i + 1;
        if (need > 1 && (!pads || atoi(pads) < need)) {
            snprintf(text, sizeof text, "%d", need);
            put_env("RECOMP_USB_PADS", text);
        }
    }
    match_commands(&m, text, sizeof text);
    put_env("RECOMP_FE_INJECT", text);
    s_session_live = 1;
    fprintf(stderr, "[NETPLAY] starting the match on slot %d: seed %u, type %d, rules %d, venue %d, fighters %d %d %d %d,"
                    " players on slots %d %d %d %d, profiles '%s' '%s' '%s' '%s', saves %s\n", m.local_slot + 1, m.seed,
            m.type, m.rules, m.venue, m.fighter[0], m.fighter[1], m.fighter[2], m.fighter[3], m.owner[0], m.owner[1],
            m.owner[2], m.owner[3], m.profile[0], m.profile[1], m.profile[2], m.profile[3],
            s_session_dir[0] ? s_session_dir : "(own)");
    fprintf(stderr, "[NETPLAY] front-end commands: %s\n", text);
    fflush(stderr);
    return 1;
}

/* ---- In game: the start barrier, inputs, the desync check ------------------------ */
#define ACTORS          0x003B92E0u /* fighter records, 0x12E8 apart */
#define STRIDE          0x12E8u
#define PAD_TABLE       0x003B8F48u /* pad-table entries, 0x20 apart, one per slot */
#define STALL_MS        10000       /* how long to wait for an input before giving up */

static long s_step;
static int s_delay = -1;
static uint32_t s_current[MAXP][ENTRY_WORDS];   /* this step's input per slot */
static long s_current_step;
static int s_desynced;

static void end_session(const char *why)
{
    fprintf(stderr, "[NETPLAY] %s; the match goes on locally\n", why);
    fflush(stderr);
    s_session_live = 0;
}

static int local_player(void)
{
    return s_match.owner[s_match.local_slot];
}

/* Has every other player reached step `step` (AT) / sent its input for it (IN)? A
 * player who has left counts as there. */
static int all_at(net_step step)
{
    int i, ok = 1, me = local_player();
    lock();
    for (i = 0; i < MAXP; i++) {
        int p = s_match.owner[i];
        if (p < 0 || p == me || s_gone_from[p] <= step) continue;
        if (s_at[p] < step) ok = 0;
    }
    unlock();
    return ok;
}

static int all_inputs(net_step step)
{
    int i, ok = 1, me = local_player();
    lock();
    for (i = 0; i < MAXP; i++) {
        int p = s_match.owner[i];
        if (p < 0 || p == me || s_gone_from[p] <= step) continue;
        if (s_input_step[p][STEP_OF(step) % RING] != step) ok = 0;
    }
    unlock();
    return ok;
}

/* Wait (with the game paused) until every other player is there; 0 after `limit_ms`
 * or when the host is gone. */
static int wait_for_all(int (*done)(net_step), net_step step, int limit_ms, int *waited)
{
    *waited = 0;
    for (;;) {
        int lost;
        if (done(step)) return 1;
        lock(); lost = s_host_lost; unlock();
        if (lost || *waited >= limit_ms) return 0;
        sleep_ms(1);
        (*waited)++;
    }
}

/* The local player's input: the pad-table entry of slot 0, which the local pad fills
 * on every game (the match binds pad 0 to slot 0). In a test run with step-timed
 * input, the script's buttons for slot 0 instead. */
static void local_input(long step, uint32_t *out)
{
    uint32_t i, buttons;
    for (i = 0; i < ENTRY_WORDS; i++)
        out[i] = MEM32(PAD_TABLE + i * 4);
    if (defjam_test_scripted_input(0, step, &buttons)) {
        memset(out, 0, ENTRY_WORDS * sizeof *out);
        out[0] = buttons;
    }
}

/* A hash of what the fight is: each fighter's state, position and health. */
static uint32_t fight_hash(void)
{
    static const uint32_t fields[][2] = {{0x30, 4}, {0x44, 12}, {0x54, 4}, {0x350, 8}};
    uint32_t h = 2166136261u, slot, f, i;
    for (slot = 0; slot < (uint32_t)s_match.slots; slot++)
        for (f = 0; f < sizeof fields / sizeof fields[0]; f++)
            for (i = 0; i < fields[f][1]; i += 4)
                h = (h ^ MEM32(ACTORS + slot * STRIDE + fields[f][0] + i)) * 16777619u;
    return h;
}

/* Every game sends its hash; a joiner compares it with the host's, the host with each
 * joiner's. The others' hashes for the previous check have had a whole interval to
 * arrive. */
static void desync_check(void)
{
    int i = (int)((s_step / SYNC_EVERY) % SYNC_KEEP);
    int prev = (int)(((s_step / SYNC_EVERY) + SYNC_KEEP - 1) % SYNC_KEEP);
    int p, me = local_player();
    uint32_t mine = fight_hash();
    send_all(-1, "SYNC %d %lld %08x", me, NET_STEP(s_net_match, s_step), mine);
    s_sync_mine[i] = mine;
    s_sync_mine_step[i] = NET_STEP(s_net_match, s_step);
    if (s_desynced) return;
    for (p = 0; p < MAXP; p++) {
        net_step peer_step;
        uint32_t peer;
        if (p == me || (!s_hosting && p != 0) || slot_of(&s_match, p) < 0) continue;
        lock();
        peer_step = s_sync_peer_step[p][prev];
        peer = s_sync_peer[p][prev];
        unlock();
        if (peer_step <= 0 || peer_step != NET_STEP(s_net_match, s_step - SYNC_EVERY)
            || s_sync_mine_step[prev] != peer_step)
            continue;
        if (peer != s_sync_mine[prev]) {
            s_desynced = 1;
            fprintf(stderr, "[NETPLAY] DESYNC: this game and player %d's differ at step %ld (%08x here, %08x there)\n",
                    p + 1, STEP_OF(peer_step), s_sync_mine[prev], peer);
        } else if (STEP_OF(peer_step) % 600 == 0) {
            fprintf(stderr, "[NETPLAY] in sync with player %d at step %ld\n", p + 1, STEP_OF(peer_step));
        }
    }
    fflush(stderr);
}

void netplay_match_start(void)
{
    int p;
    s_step = 0;
    s_current_step = 0;
    s_desynced = 0;
    if (s_inited) lock();
    s_net_match++;
    for (p = 0; p < MAXP; p++) s_vote[p] = -1;
    if (s_inited) unlock();
}

void netplay_step(void)
{
    int me = local_player(), waited, slot;
    uint32_t mine[ENTRY_WORDS];
    if (!s_session_live) return;
    if (s_delay < 0) {
        const char *d = getenv("RECOMP_NETPLAY_DELAY");
        s_delay = d && *d ? atoi(d) : 3;
        if (s_delay < 1) s_delay = 1;
        if (s_delay > 30) s_delay = 30;
    }
    s_step++;
    if (s_step == 1) {
        /* Every game is at the first step of the same match; whichever got here first
         * waits for the others, so the fight starts on all of them together. */
        send_all(-1, "AT %d %lld", me, NET_STEP(s_net_match, 1));
        fprintf(stderr, "[NETPLAY] at the first step of match %d; waiting for the other players\n", s_net_match);
        fflush(stderr);
        if (!wait_for_all(all_at, NET_STEP(s_net_match, 1), BARRIER_MS, &waited)) {
            end_session(s_host_lost ? "the host disconnected" : "not every player arrived");
            return;
        }
        fprintf(stderr, "[NETPLAY] every player at the first step after %d ms: fight (input delay %d steps)\n",
                waited, s_delay);
        fflush(stderr);
    }

    /* This step's local input is played `delay` steps from now, on every game. */
    local_input(s_step, mine);
    store_input(me, NET_STEP(s_net_match, s_step + s_delay), mine);
    send_all(-1, "IN %d %lld %08x %08x %08x %08x %08x %08x", me, NET_STEP(s_net_match, s_step + s_delay),
             mine[0], mine[1], mine[2], mine[3], mine[4], mine[5]);

    /* The first `delay` steps have no inputs anywhere: every game plays them empty. */
    memset(s_current, 0, sizeof s_current);
    if (s_step > s_delay) {
        if (!wait_for_all(all_inputs, NET_STEP(s_net_match, s_step), STALL_MS, &waited)) {
            end_session(s_host_lost ? "the host disconnected" : "a player stopped sending");
            return;
        }
        if (waited > 50)
            fprintf(stderr, "[NETPLAY] waited %d ms for the other players' input at step %ld\n", waited, s_step);
        lock();
        for (slot = 0; slot < MAXP; slot++) {
            int p = s_match.owner[slot];
            if (p >= 0 && s_gone_from[p] > NET_STEP(s_net_match, s_step))
                memcpy(s_current[slot], s_inputs[p][s_step % RING], sizeof s_current[slot]);
        }
        unlock();
    }
    s_current_step = s_step;
    if (s_step % SYNC_EVERY == 0)
        desync_check();
}

/* The game's XInputGetState (sub_0027E075, docs/research/m4b-controller-ports.md) has
 * just filled an XINPUT_STATE. Start opens the pause menu, which stops the fight
 * steps on this game only; the others then wait for inputs that never come. So while
 * a netplay fight runs (from the first step until the match is decided, phase 2 at
 * 0x3B8F30) Start is taken out of the buttons word (+4, bit 0x10). */
#define G_PHASE         0x003B8F30u
#define XINPUT_START    0x0010u

void netplay_filter_pad(uint32_t state)
{
    static int was_down;
    int down;
    if (!s_session_live || !s_current_step || !state || MEM32(G_PHASE) >= 2u) return;
    down = (MEM16(state + 4) & XINPUT_START) != 0;
    if (down && !was_down) {
        fprintf(stderr, "[NETPLAY] Start held back at step %ld: pausing is off in a netplay fight\n", s_step);
        fflush(stderr);
    }
    was_down = down;
    MEM16(state + 4) = (uint16_t)(MEM16(state + 4) & ~XINPUT_START);
}

/* sub_001BAA00(record): a fighter is about to read its pad-table entry. Every game puts
 * the same exchanged input there for each player's fighter; the CPU's are left alone. */
void netplay_input(uint32_t actor)
{
    uint32_t slot, i;
    if (!s_session_live || !s_current_step) return;
    if (actor < ACTORS || actor >= ACTORS + MAXP * STRIDE || (actor - ACTORS) % STRIDE) return;
    slot = (actor - ACTORS) / STRIDE;
    if (slot >= (uint32_t)s_match.slots || s_match.owner[slot] < 0) return;
    for (i = 0; i < ENTRY_WORDS; i++)
        MEM32(PAD_TABLE + slot * 0x20u + i * 4) = s_current[slot][i];
}

/* ---- After a match: rematch or quit, together ------------------------------------
 * Each player's menus after a match are their own, but a rematch is a netplay match
 * again and needs every game. So in a session the front end's Game.Rematch() is held
 * back (netplay_hold_call, from the FuncCall hook) and this game's vote sent; once
 * every player still in the session has chosen it, every game makes the call itself
 * (netplay_tick, on the game thread) and the new match starts on all of them. Whoever
 * quits instead (Game.Quit()) leaves the session, and every game still waiting is sent
 * to the menus with the same call. */
static int s_rematch_waiting;           /* this game's Game.Rematch() is held */
static char s_banner[96];
static unsigned long long s_banner_until;

static unsigned long long now_ms(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000u + (unsigned long long)t.tv_nsec / 1000000u;
#endif
}

static void banner(const char *text, unsigned ms)
{
    if (s_inited) lock();
    snprintf(s_banner, sizeof s_banner, "%s", text);
    s_banner_until = ms ? now_ms() + ms : ~0ull;
    if (s_inited) unlock();
}

const char *netplay_banner(void)
{
    static char copy[96];
    if (!s_banner[0]) return NULL;
    if (s_inited) lock();
    if (now_ms() > s_banner_until) s_banner[0] = 0;
    snprintf(copy, sizeof copy, "%s", s_banner);
    if (s_inited) unlock();
    return copy[0] ? copy : NULL;
}

static void vote(int choice)
{
    int me = local_player();
    lock();
    s_vote[me] = choice;
    unlock();
    send_all(-1, "VOTE %d %d %d", me, s_net_match, choice);
    fprintf(stderr, "[NETPLAY] this player chose %s after match %d\n", choice ? "rematch" : "quit", s_net_match);
    fflush(stderr);
}

int netplay_quick_start(void)
{
    return s_session_live;
}

int netplay_hold_call(const char *text)
{
    if (!s_session_live || !s_net_match || !text) return 0;
    if (strstr(text, "Game.Rematch()")) {
        if (!s_rematch_waiting) vote(1);
        s_rematch_waiting = 1;
        banner("Rematch: waiting for the other players to choose it...", 0);
        return 1;
    }
    if (strstr(text, "Game.Quit()")) {
        vote(0);
        s_session_live = 0;
        s_rematch_waiting = 0;
        banner("You left the online session.", 4000);
        return 0;
    }
    return 0;
}

void netplay_tick(void)
{
    extern void fe_inject_command(const char *command);
    int p, all = 1, declined = 0, me;
    if (!s_rematch_waiting) return;
    me = local_player();
    lock();
    for (p = 0; p < MAXP; p++) {
        if (p == me || slot_of(&s_match, p) < 0) continue;
        if (s_gone_from[p] != NO_STEP || (!s_hosting && s_host_lost) || s_vote[p] == 0) declined = 1;
        else if (s_vote[p] != 1) all = 0;
    }
    unlock();
    if (declined) {
        s_rematch_waiting = 0;
        s_session_live = 0;
        fprintf(stderr, "[NETPLAY] a player quit or left: no rematch\n");
        fflush(stderr);
        banner("No rematch: another player quit. Back to the menu.", 5000);
        fe_inject_command("Game.Quit()");
    } else if (all) {
        s_rematch_waiting = 0;
        fprintf(stderr, "[NETPLAY] every player chose rematch: match %d starts\n", s_net_match + 1);
        fflush(stderr);
        banner("Rematch!", 2000);
        fe_inject_command("Game.Rematch()");
    }
}
