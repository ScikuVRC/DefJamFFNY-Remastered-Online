# Netplay

Up to four players, one match, over the internet or a LAN. The design follows Dolphin's
netplay: the games exchange only what the players do, never game state, and rely on
lockstep mode (docs/lockstep-mode.md) to play the same fight from the same inputs.

| Step | State |
|---|---|
| 1. Lockstep mode: the same inputs and seed play the same fight on any frame rate | done |
| 2. Desync check: a per-step fingerprint of the fight (`RECOMP_TEST_SYNC`) | done, test runs only |
| 3. Match setup: lobby, compatibility check, agreed match, start barrier | done |
| 4. Lockstep play: exchange inputs every step, input delay, live desync check | done; untested with a real pad and between machines |
| 5. Created fighters: each player's own, from their profile | done (one profile tested) |
| 6. The host's port opened in the router (UPnP) | done; tested on one router, not yet from outside it |
| 7. Rollback, spectating, a relay for players behind a router UPnP cannot open | later |

## Playing

In the launcher, the **Online** tile (**Online Match** on the compact Home). One player hosts (a port, 7777 by default), the others
join with the host's **invite code**, or with `host-address:port`. For players over the
internet the lobby asks the router to forward the port to the host (below); it then shows the
invite code, with **Copy**, and the host's addresses on the local network and over the
internet.

### Invite codes

An invite code (`netplay_invite.c`) is the lobby's addresses in one line to send: the port,
the host's public address (when the router opened the port) and its address on its home
network, with a CRC-8, in Crockford base32 after `DJ-` (`DJ-XXXX-XXXX-XXXX-XXXX-XXXX`, 20
letters with both addresses). No server is involved, so a code changes when the host's
public address does. Joining with one tries the home-network address first (a friend in the
same house, whom the router may not let back in through its public address), then the public
one, three seconds each, over and over until the host answers or Leave is pressed. Case, spaces
and dashes do not matter, and O reads as 0, I and L as 1; a mistyped letter fails the
checksum, and the field is then taken as an address. Without the router (it could not open the
port, or `RECOMP_NETPLAY_UPNP=0`), the code has the home-network address only.
`RECOMP_NETPLAY_JOIN` takes a code too.

### The router

Hosting asks the home router, through UPnP's Internet Gateway Device, to forward the TCP
port to this PC (`src/hooks/netplay_router.c`): an SSDP search out of every network adapter
(a VPN or a virtual switch often takes multicast otherwise), the router's description, then
`AddPortMapping` and `GetExternalIPAddress`. The forward is taken away when the session ends,
when the launcher or the game window is closed, and by the router itself after four hours
if the game never gets to (a crash); a router that takes only permanent forwards gets one.
When it cannot be done the lobby says why, and the port has to be forwarded by hand:

- no router answered (UPnP switched off in the router: switch it on, or forward the port);
- the port is already forwarded to another device on the network;
- the router is behind another one (its own address is private, or a provider's shared
  `100.64.x.x` one): a forward there reaches only the network outside it. Forward the port
  in both, or play over a virtual LAN such as a VPN the players share.

`RECOMP_NETPLAY_UPNP=0` leaves the router alone; test runs (`RECOMP_SETTINGS=none`) do too
unless it is `1`. `RECOMP_NETPLAY_UPNP_LOCATION=<description URL>` skips the search
(`tests/unit/test_netplay_router.py` runs the code against a fake router that way). Tested
2026-11-03 on one router (a Sagemcom): opened, reachable at the public address from the
same network, and gone again after leaving the lobby; not yet joined from outside it.

1. **Waiting room.** Everyone who joins is listed; each player ticks **I'm ready**.
   The host chooses the match: the type (One on One, Team Match, Free For All and the
   special matches: Window, Subway, Demolition, Inferno, Cage, Ring Out), the rules
   and the venue (the special types have their own). When everyone is ready the host
   presses **Pick fighters**.
2. **Fighter select.** Each player picks their own fighter: any of the game's, or
   **Created: <profile>**, the created fighter of one of their own profiles (the
   saves in their data folder). The host picks the CPU's fighters for free slots. A
   game fighter can be in a match once; created fighters can share a profile name
   (each player's is their own). Each player ticks **Lock in my
   fighter**; when everyone has, the host presses **Start match**.
3. Every launcher closes and every game goes straight into the match: the legal screen
   and the startup movies are skipped whatever the skip-intro setting, and on the title
   screen Start and A are pressed for the player every second (Start is taken about 7 s
   in; A clears the memory-card notice) until the main menu, where the match's commands
   start it at once. The player's own pad works throughout. (A test run's own pad
   script, `RECOMP_PAD_SCRIPT`, is left as it is.)

Start is held back during a netplay fight (pausing would stop one game only).

## Rematch

After the match each player goes through the summary to the game's **REMATCH? YES /
NO** box. A rematch is a netplay match again, so it starts only when every player has
chosen YES: whoever picks YES first sees "Rematch: waiting for the other players to
choose it..." at the top of the picture, and the game holds its `Game.Rematch()` back
(the FuncCall hook asks `netplay_hold_call`) and sends `VOTE <player> <match> 1`. When
every player still in the session has voted YES, every game makes the call itself and
the next match starts on all of them together (the start barrier is per match). If
anyone picks NO (`Game.Quit()`, after the tip screen) or leaves, every game still
waiting is sent to the menus with the same call, and the session ends. Steps on the
wire carry their match (`match << 24 | step`), so a rematch never takes the last
match's messages for its own.

Before this (2026-11-01) a YES started the rematch on that game alone: the start
barrier still held the last match's marks, and the game played on by itself after 10 s
without the others' inputs.

Tested (two games, one PC, harness routes `netplay-rematch` and `netplay-quit`): YES and
NO sent the YES side back to the match-type menu without a match; YES and YES started
match 2 on both together, which stayed in sync to the same result.

## Your record and replays

Every online match goes into the player's online record, kept in `<data>/online`
(`src/hooks/netplay_record.c`), which the launcher's Online page shows under Host and
Join: the player's online name, a rank title (by counted wins: Rookie, Contender at 5,
Brawler at 15, Enforcer at 30, Kingpin at 60, Legend of the city at 100), the record,
the win rate, the current streak, the favourite fighter and the best venue, with
**Match history** and **Rivals** behind two buttons.

- `player.ini` holds this copy's player ID, 16 hex digits made once. It travels in
  `HELLO` and the host's `IDS` line, so each game keeps its record against the others
  by ID: a new name does not split a rival in two.
- `history.txt` has one line per match: when, the result as this player sees it (win,
  loss, draw, or unfinished), the type, the venue, their fighter, the replay's file and
  the other players (names and IDs; a Team Match's partner is not an opponent). The
  result comes from the game's own result recorder (`sub_001A6A80`, the decided call:
  code bit 0, a draw with bit 2). A match another player left before it was decided is
  listed as unfinished and not counted: the one still there would have won against an
  empty pad. A match with no other player (a host alone, against the CPU) is listed as
  practice and not counted.
- `replays/<date>-<time>-<match>.djr` is the match itself, written as it plays: the
  build, the players, the `MATCH` line, every created fighter's save as it was sent,
  then each step a slot's input changed, the fight hash every 60 steps, the result and
  the last step. The newest 100 are kept.

**Watch** in the history starts the game straight into the replay: it plays as an
online match with one game and no network, from the same seed, the same saves (each
created fighter added as a guest, as another player's would be, since the player's own
may have changed since) and the recorded inputs. On the way it checks the fight against
the recorded hashes and says so on screen if the two part ("This replay no longer plays
the same fight"). A replay plays only on the build that recorded it, because another
build may play a different fight from the same inputs; the history says why a Watch
button is off. In a replay Start pauses as usual, and the game's Rematch watches it
again. There is no fast-forward or seeking yet.

## Created fighters

A created fighter is part of its profile's save, so the profile has to be on every
game. When the host starts, a player with a created fighter sends that profile's save
folder to the host, and the host sends every player every created fighter's profile.

Each game plays from its player's **own saves**: their profiles, their options and
unlocks, and whatever the game saves during and after the session (earnings, unlocks,
the options) stays theirs. The other players' profiles are added to the player's save
folder for the session (`place_saves`), listed in `<data>/save/netplay-guests.txt`, and
taken out when the game closes, or at the next start after a crash. A profile the
player already has with the same contents is used as it is. Profiles may share a name:
two friends can both be MARC. A game never renames its own player's profile; another
player's whose name it already has (its player's own MARC, or another MARC of the match)
is added under another name, MARC2, MARC3 and so on (`netplay_saves.c`: the name in the
save, its `SaveMeta.xbx` and its folder, and the save's signatures). So on each screen
your fighter has your profile's name and the other may show as MARC2; the fight is the
same (checked with two games whose MARCs differ: 2026-11-04).

A slot's profile is a user number in the game's list, which follows the order of the
save folders (checked with three profiles whose folder and name orders differ:
2026-11-02), so each game finds the number in its own save folder. Different options
and unlocks on each game do not part the fight: a pair whose joiner had every fighter
and venue unlocked, and its own volume, stayed in sync to the same result.

The front end sets a created fighter up in steps that take time, and the commands
follow the user-ID and fighter screens as a player goes through them
(`match_commands` in netplay.c; `+F` waits F frames, fe_inject.c):

```
mainmenu      => StartBattleMode; SetMatchType; SetMatchRules; ControllerBind(-1);
                 MemCard.ChooseMCDevice; ControllerBind(0); MemCard.GetNumUsers;
                 MemCard.GetUserData(user) for each profile
mainmenu +120 => MemCard.SetBMCurrentUserIndex(slot, user); Battle.LoadCreatePlayerData(n);
                 Game.ContinueLoad(n)                       (opens the fighter screen)
getattributesbm(-2 +300 =>  ControllerSetup(slot, ..., 99, ...); ChooseVenue; GetMatchInfo;
                 Battle.SetCPViewportCam(n, 1 or 5); GetCreatePlayerName; StartGame
```

Without `GetUserData` the profile is not bound to the slot (the fighter screen shows a
stock fighter); without the pauses the match loads to an empty arena.

A created fighter's skeleton is read back from what was last drawn, so in lockstep
the main loop runs at most one fight step per presented frame (lockstep.c); without
that, two games with the same inputs parted a few hundred steps in. The shared random
generator the menus draw from (`0x33DB5C`) is also seeded from the lockstep seed when
the match is set up: a created fighter's intro is drawn from it.

## The session (src/hooks/netplay.c)

The host is the hub: every joiner has one TCP connection, to the host, which passes
each joiner's in-game messages on to the others. Text lines (protocol 3):

1. joiner: `HELLO <protocol> <version> <program> <data> <name> <id>`; host: `WELCOME
   <player>` or `REFUSE <reason>`. `<id>` is the player's ID (Your record and replays). The fingerprints are hashes of the running program
   and of `game/default.xbe`; any difference refuses the player with the reason,
   because a different build or dump plays a different fight.
2. In the lobby the host sends `LOBBY <type> <rules> <venue> <f0..f3> <slots> <players>
   <phase> <ready mask> <name0..3> <profile0..3>` whenever it changes; a joiner sends
   `READYUP <0|1>` and `PICK <fighter> [<profile>]` (profiles in hex; `-` is none).
3. The host starts: `IDS <id0..3>`, then `MATCH <seed> <type> <rules> <venue> <slots> <f0..f3> <t0..t3>
   <o0..o3> <profile0..3>`. The saves follow (`FILE <player> <folder> <name> <size>`, `DATA
   <hex>`..., `END`; a joiner ends its own with `SENT`, the host its with `FILES`), then
   each joiner `READY` and the host `GO`.
4. Every game sets lockstep mode with that seed, the session's save folder and the
   front-end commands that start the match from the main menu (`RECOMP_FE_INJECT`,
   src/hooks/fe_inject.c): `Game.StartBattleMode`, `SetMatchType`, `SetMatchRules`,
   `ControllerSetup` per slot (the slot is its own controller), `ControllerSelect` for
   a Team Match's places, `ChooseVenue`, `StartGame` (with created fighters, the steps
   above). No menu is driven by pad presses, so nothing depends on timing.
5. In the game, at the first fight step each sends `AT <player> 1` and waits for every
   other (up to two minutes), so the fight starts on all together whatever their
   loading times.

Player 2's slot is set up as the CPU: a human slot 2 needs a second pad plugged in,
and match set-up crashed without one (`sub_001BBC10` on slot 2's record). Step 4
replaces each fighter's pad word every step from the exchanged inputs (the write the
step-timed test input already makes), which drives a CPU slot too.

## Inputs (step 4)

Lockstep with an input delay, as in Dolphin. At every fight step each game:

1. reads its player's input: the first six words of slot 0's pad-table entry
   (`0x3B8F48`; buttons, the change mask and both sticks, which is everything
   `sub_001BAA00` reads), which the local pad fills on both machines;
2. sends it for `delay` steps ahead (`IN <step> <6 words>`; `RECOMP_NETPLAY_DELAY`,
   default 3 steps, 50 ms) and keeps it for that step itself;
3. waits until it has the other player's input for this step (up to 10 s, then the
   match goes on locally), and puts both players' inputs in both fighters' entries
   just before each reads its own (the write the step-timed test input makes; it
   drives player 2's CPU slot as well).

The first `delay` steps have no inputs on either side. Every 60 steps both games send
`SYNC <step> <hash>` of the fighters' state, position and health, and each compares
the other's with its own; a difference is logged as `[NETPLAY] DESYNC`.

In a test run with step-timed input (`--step-input`), the script's buttons for slot 0
are the local player's input instead of the pad.

## Scripted sessions

`RECOMP_NETPLAY_HOST=<port>` or `RECOMP_NETPLAY_JOIN=<host:port>` run the same
session headlessly at start-up (`netplay_startup`, after the launcher), and
`RECOMP_NETPLAY_SEED` fixes the host's seed, `RECOMP_NETPLAY_VENUE` (a ChooseVenue
index) and `RECOMP_NETPLAY_FIGHTERS` (`f0,f1`) the host's match. The harness route `netplay` boots with
no menu presses and ends at the match summary:

```
python scripts/scenario_suite.py run netplay --fixture F --no-audio --step-input default \
    --test-env RECOMP_TEST_SYNC=1 --test-env RECOMP_NETPLAY_HOST=7777 --output A
python scripts/scenario_suite.py run netplay --fixture F --no-audio --step-input default \
    --test-env RECOMP_TEST_SYNC=1 --test-env RECOMP_NETPLAY_JOIN=127.0.0.1:7777 --output B
python scripts/sync_diff.py A/game.log.err B/game.log.err
```

Measured (2026-10-29, two games on one PC): both agreed the seed, started the match
from the main menu and met at the first step (the second to load waited 1.4 to 3.4 s
for the first). With different step-timed inputs (host: right and attacks; joiner:
left and button 0x20) both games played the same 8-minute fight, all 28,684 steps, to
the same result; player 2 followed the joiner's input on both, not the CPU's. The
desync check agreed at all 48 points, no step waited 50 ms for an input, and the
games ended 31 ms apart.

With the live pad instead (the emulated USB pad, the path a real pad takes; harness
routes `netplay-pad` and `netplay-pad-left`, presses timed by the host's clock), the
two games played the same 12-minute fight, all 43,280 steps, to the same result, with
the desync check agreeing at all 73 points and no input stall: the local input read
from the pad-table entry is the pad's, not an echo. A session the host set to the
Terrordome (`RECOMP_NETPLAY_VENUE=6`) started there on both and stayed in sync.

## Known gaps

- Untested between two machines, over the internet, and with a physical pad (the
  emulated USB pad it was tested with is the same path).
- Pausing: in a netplay fight Start is taken out of what the game's XInputGetState
  (`sub_0027E075`) returns, from the first step until the match is decided, and
  logged as `Start held back`. Unproven: in the matches the session starts, a Start
  press on the live pad (harness route `netplay-pause`, and the same match without
  netplay) opened no pause menu at all, so the filter never had anything to hold
  back. The steps that did stop in an earlier probe were a first-time tip, which
  lockstep mode turns off.
- Player 2's slot is still the CPU's in the game's own set-up (its name on screen,
  its AI still running under the input).
- TCP: a lost packet stalls both games until it is resent. UDP and rollback later.
- Three- and four-player matches (Team Match, Free For All) and created fighters in
  them are untested over the network; so is a match with two different players'
  created fighters (the game's profile order itself was checked).
- Every player needs the same file mods (docs/mod-manager.md#file-mods).
- Replays (2026-11-05): a host alone against the CPU, and both sides of a two-game
  session, played back to the recorded result at the same step with every hash
  check agreeing (22 to 40 checks per match), from the command line
  (`RECOMP_NETPLAY_REPLAY`) and from the launcher's Watch. Unverified: a replay of a
  session's second match (a rematch), which plays from a fresh start while the
  recording followed a first match; the hash check says on screen if one plays
  differently. The harness route `netplay-rematch` now stops at a tip screen after
  the first match (no stage presses through it), so that test needs a new route.
- The intermittent start-up stall (stuck at "press start": the USB host controller's
  first deferred routine ran seconds late and the driver gave up on the pad) is fixed
  in the toolkit: the timer thread, pinned to the guest core, went unscheduled for
  3.1 s in about one start in fifteen; it now only prefers that core. 0 stalls in 65
  starts against 4 in 55. The fix is commit `6cc6238` on the toolkit branch
  `defjam/timer-ideal-core` of the fork `ScikuVRC/xboxrecomp`, which `tools/xboxrecomp`
  now follows (merged with upstream's toolkit at `d569ca1`; file mods use upstream's
  `g_xbox_game_file_redirect`). Two games starting
  at once on one PC still stalled now and then (about one pair start in four), and more
  often since the v0.13.0 merge: 3 of 4 pair starts (the host each time) hung before the
  first frame, with the timer thread not run for minutes and the USB and graphics DPCs
  queued. One game alone started 14 of 14 times, and the pair that started played
  11,655 steps with fighters, pads and generators the same on both (only the background
  crowd differed, as it did before the merge: docs/research/crowd-nondeterminism.md; it
  agrees since v0.14.2).
- A rare wild indirect call in the controller library at boot (`0x27EC6E`, once in 69
  runs), skipped without harm.
