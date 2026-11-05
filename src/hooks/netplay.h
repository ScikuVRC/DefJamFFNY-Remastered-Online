#ifndef DEFJAM_NETPLAY_H
#define DEFJAM_NETPLAY_H
/* Netplay sessions: up to four players meet in the host's lobby over TCP, agree on a
 * match, every game starts it in lockstep mode (lockstep.h), and the fight begins on
 * all of them at once (src/hooks/netplay.c, docs/netplay.md). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETPLAY_DEFAULT_PORT 7777
#define NETPLAY_MAX_PLAYERS 4
#define NETPLAY_NAME_LEN 16
#define NETPLAY_PROFILE_LEN 32
/* ControllerSetup's fighter for a profile's created fighter (the user-ID screen binds
 * the profile to the slot first). */
#define NETPLAY_CREATED_FIGHTER 99

/* The lobby's phases: players gather and say they are ready, then pick their
 * fighters and lock them in; the host moves the lobby on and starts the match. */
#define NETPLAY_PHASE_WAITING 0
#define NETPLAY_PHASE_PICKING 1

typedef enum {
    NETPLAY_IDLE,
    NETPLAY_LISTENING,      /* host: the lobby is open */
    NETPLAY_CONNECTING,     /* join: reaching the host */
    NETPLAY_HANDSHAKE,      /* connected; comparing builds */
    NETPLAY_LOBBY,          /* join: in the host's lobby, waiting for the start */
    NETPLAY_READY,          /* match agreed; start the game */
    NETPLAY_FAILED          /* see netplay_status() */
} NetplayState;

/* One match: what every game hands its front end, the same everywhere. */
typedef struct {
    uint32_t seed;          /* RECOMP_LOCKSTEP_SEED on every game */
    int type;               /* Game.SetMatchType */
    int rules;              /* Game.SetMatchRules */
    int venue;              /* Game.ChooseVenue */
    int slots;              /* fighters in the match: 2 or 4 */
    int fighter[NETPLAY_MAX_PLAYERS];   /* Controller.ControllerSetup's fighter per slot */
    int team[NETPLAY_MAX_PLAYERS];      /* ... and its team */
    int owner[NETPLAY_MAX_PLAYERS];     /* the player on each slot, -1 for the CPU */
    /* For NETPLAY_CREATED_FIGHTER: the profile (its save's name) whose created fighter
     * the slot has; the owner's own, sent to every game when the match starts. */
    char profile[NETPLAY_MAX_PLAYERS][NETPLAY_PROFILE_LEN];
    int local_slot;         /* the slot this game's player has */
} NetplayMatch;

/* The lobby as every game sees it. Player 0 is the host; slot i is player i's. */
typedef struct {
    int players;                                    /* connected, host included */
    int local_player;                               /* this game's player, -1 outside a lobby */
    int phase;                                      /* NETPLAY_PHASE_* */
    int ready[NETPLAY_MAX_PLAYERS];                 /* per player: ready, or locked in when picking */
    char name[NETPLAY_MAX_PLAYERS][NETPLAY_NAME_LEN];
    NetplayMatch match;                             /* the host's settings so far */
} NetplayLobby;

/* Battle's match types (netplay_catalog.c). */
typedef struct {
    int type;               /* Game.SetMatchType */
    const char *name;
    int fighters;           /* 2 or 4 */
    int venue;              /* the type's own venue, or -1 when the host picks one */
    int rules;              /* 1: the rules screen (Game.SetMatchRules) applies */
} NetplayMatchType;

/* A fighter (ControllerSetup's number) or a venue (ChooseVenue's), with its name. */
typedef struct {
    int id;
    char name[48];
} NetplayChoice;

int netplay_match_types(const NetplayMatchType **out);
const NetplayMatchType *netplay_match_type(int type);
int netplay_fighters(const NetplayChoice **out);    /* from game/fighter.xml, by name */
int netplay_venues(const NetplayChoice **out);      /* from game/venue.xml, by name */
const char *netplay_choice_name(const NetplayChoice *list, int count, int id);
/* The folder this player's saves are in (the parent of UserData/45410049), and the
 * profiles there: each has a created fighter. Returns how many were found. */
int netplay_save_root(char *out, int size);
int netplay_profiles(char names[][NETPLAY_PROFILE_LEN], int max);
/* For netplay.c: a folder's files or sub-folders, sorted (up to `max`); whether a file
 * is a save's own (not the dashboard's SaveMeta.xbx or saveimage.xbx); the folder under
 * <root>/UserData/45410049 that holds the save `name`. */
int netplay_list_dir(const char *dir, int dirs, char names[][64], int max);
int netplay_save_file(const char *name);
int netplay_save_folder(const char *root, const char *name, char folder[64]);

/* Start a session in the background; poll netplay_state() and netplay_lobby(). */
void netplay_set_name(const char *name);
int netplay_host(uint16_t port);
int netplay_join(const char *address);     /* "host[:port]" */
void netplay_cancel(void);

/* Host: change the match (type, rules, venue, a slot's fighter or team). Sent to
 * everyone in the lobby. Slot fighters of other players are theirs to pick. */
void netplay_set_match(int type, int rules, int venue);
void netplay_set_slot(int slot, int fighter, int team);
/* Any player: the fighter for their own slot, or the created fighter of one of their
 * profiles (netplay_profiles). */
void netplay_pick(int fighter);
void netplay_pick_created(const char *profile);
/* Any player: ready (in the waiting room) or locked in (when picking), or not. */
void netplay_set_ready(int ready);
/* Host: the lobby's phase; every player's ready flag is cleared. */
void netplay_set_phase(int phase);
/* Host: start with the lobby as it is. Returns 0 (and says why in the status) when
 * the players do not fit the match type or have not all locked in. */
int netplay_start(void);
/* This computer's IPv4 addresses, for the host to tell the others. */
int netplay_local_addresses(char out[][48], int max);
/* Host: the settings a scripted session starts with (RECOMP_NETPLAY_*). */
void netplay_choose(int venue, int fighter0, int fighter1);

NetplayState netplay_state(void);
/* A line for the player: what the session is doing, or why it failed. */
void netplay_status(char *out, int size);
/* The lobby (players, names, settings); 0 outside a session. */
int netplay_lobby(NetplayLobby *out);
/* The agreed match, once the state is NETPLAY_READY. */
int netplay_match(NetplayMatch *out);

/* Called after the launcher, before the game reads its environment. Runs a session
 * from RECOMP_NETPLAY_HOST=<port> / RECOMP_NETPLAY_JOIN=<host:port> when one is set
 * (blocking, for scripted runs; RECOMP_NETPLAY_PLAYERS is how many the host waits
 * for), and, once a session is ready, sets lockstep mode, its seed and the front-end
 * commands that start the agreed match. Returns 0 when a session was asked for and
 * failed. */
int netplay_startup(void);

/* From the game: a match is being set up / a fight step is about to run. At the
 * first step each game waits until all have reached it; at every step each sends
 * its local input for `delay` steps ahead (RECOMP_NETPLAY_DELAY, default 3) and
 * waits for every other player's input for this one. */
void netplay_match_start(void);
void netplay_step(void);
/* sub_001BAA00(record): put the exchanged input in the fighter's pad-table entry. */
void netplay_input(uint32_t actor);
/* After the game's XInputGetState filled `state`: no Start (pause) in a netplay fight. */
void netplay_filter_pad(uint32_t state);
/* The FuncCall hook, before the front end's handler: 1 to hold the call back (a
 * rematch waiting for the other players' votes). */
int netplay_hold_call(const char *text);
/* This process starts an online match: skip the intro and the title (skip_intro.c). */
int netplay_quick_start(void);
/* Once a game step (the run-module manager's tick): a held rematch starts, or turns
 * into a quit, when every player has voted. */
void netplay_tick(void);
/* A line for the screen while something waits on the session, or NULL. */
const char *netplay_banner(void);

/* The game's result recorder (sub_001A6A80) was called: the slot lists (each ended by -1)
 * and the code. A decided online match goes to the player's record (netplay_record.h). */
void netplay_result(uint32_t winners, uint32_t losers, uint32_t code);

/* Replays (netplay.c, docs/netplay.md): every online match is recorded to
 * <data>/online/replays. Whether this build can play the replay at `path` (and why not, or
 * what it is, in `why`); the one to play at the next start (netplay_startup, after the
 * launcher; "" none; RECOMP_NETPLAY_REPLAY does the same for a scripted run); and whether
 * this process plays one. */
int netplay_replay_check(const char *path, char *why, int size);
void netplay_replay_choose(const char *path);
int netplay_replaying(void);

#ifdef __cplusplus
}
#endif
#endif
