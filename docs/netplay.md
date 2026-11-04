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

In the launcher, **Online Match**. One player hosts (a port, 7777 by default), the others
join with `host-address:port`. The lobby shows the host's addresses on the local network,
and for players over the internet it asks the router to forward the port to the host
(below) and shows the public address to give them, with **Copy**.

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
   fighter (or a profile) can be in a match once. Each player ticks **Lock in my
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

## Created fighters

A created fighter is part of its profile's save, so the profile has to be on every
game. When the host starts, a player with a created fighter sends that profile's save
folder to the host, and the host sends every player every created fighter's profile.

Each game plays from its player's **own saves**: their profiles, their options and
unlocks, and whatever the game saves during and after the session (earnings, unlocks,
the options) stays theirs. The other players' profiles are added to the player's save
folder for the session (`place_saves`), listed in `<data>/save/netplay-guests.txt`, and
taken out when the game closes, or at the next start after a crash. A profile the
player already has with the same contents is used as it is; one with the same name and
other contents is the player's own save, and then, so as to touch neither, that game
plays the match from a session folder of its own (`<data>/save/netplay-session-<player>`,
`RECOMP_SAVE_DIR`: the match's profiles and the player's own options).

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
each joiner's in-game messages on to the others. Text lines (protocol 2):

1. joiner: `HELLO <protocol> <version> <program> <data> <name>`; host: `WELCOME
   <player>` or `REFUSE <reason>`. The fingerprints are hashes of the running program
   and of `game/default.xbe`; any difference refuses the player with the reason,
   because a different build or dump plays a different fight.
2. In the lobby the host sends `LOBBY <type> <rules> <venue> <f0..f3> <slots> <players>
   <phase> <ready mask> <name0..3> <profile0..3>` whenever it changes; a joiner sends
   `READYUP <0|1>` and `PICK <fighter> [<profile>]` (profiles in hex; `-` is none).
3. The host starts: `MATCH <seed> <type> <rules> <venue> <slots> <f0..f3> <t0..t3>
   <o0..o3> <profile0..3>`. The saves follow (`FILE <folder> <name> <size>`, `DATA
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
  crowd differs, as it did before the merge: docs/research/crowd-nondeterminism.md).
- A rare wild indirect call in the controller library at boot (`0x27EC6E`, once in 69
  runs), skipped without harm.
