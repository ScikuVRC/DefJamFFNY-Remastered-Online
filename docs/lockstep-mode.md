# Lockstep mode

`RECOMP_LOCKSTEP=1` makes a fight a function of its inputs and one seed: two copies
of the game given the same inputs play the same fight, step for step. Netplay is
built on it (inputs are exchanged, never game state). Off by default; a normal game
is not changed.

```powershell
$env:RECOMP_LOCKSTEP = "1"
$env:RECOMP_LOCKSTEP_SEED = "20261006"   # optional; both machines must use the same one
```

## What it changes

| Source of difference | Fix | Code |
|---|---|---|
| Match generator: seed, and for a process's later matches an additive term from the time-stamp counter | both pinned | `lockstep.c`, `test_telemetry.c` (`defjam_test_rng`) |
| Four per-fighter AI generators seeded from the time-stamp counter | pinned | `defjam_test_ai_seed` |
| CLogic's generator (`0x376FDC`) seeded from the time-stamp counter | pinned where it is seeded | `crowd_control.c` (`defjam_crowd_seed`) |
| Background crowd and spectators leave the loading window in a host-timed state; both update once a frame; they share generators with menus | `hold`, `spect`, `reseed`; in lockstep the background crowd also runs 4 set-up updates in the window and the rest at the first step, gets its time once a fight step, and draws from a private `rand` seed (v0.14.2) | `crowd_control.c`, docs/research/crowd-nondeterminism.md |
| First-time help tips (CFirstTimeHelp at `0x346A68`) pause the fight, and which ones a player gets depends on their save | off: the enabled byte `0x346BEC` is cleared whenever the help hears of an event (`sub_000D6670`) | `defjam_lockstep_tip` |
| The fight step draws from the utility generator (`0x39C0E0`, through `sub_000AC590`), which UI code also draws from every frame | the step gets a private copy, seeded at every match start | `defjam_lockstep_utility_swap` |

Every seed derives from `RECOMP_LOCKSTEP_SEED` (default 20261006).

## How it was checked

Pairs of full pinned matches, the second of each with vsync off and render scale 1
(a different frame rate, standing in for a different machine), compared step by step
with `RECOMP_TEST_SYNC=1` and `scripts/sync_diff.py` (docs/09-testing-harness.md).
The fighters, AI generators, match generator, the step's utility generator and the
spectators agreed at every step:

| Match | Steps compared | Result |
|---|---|---|
| One on One, default venue | 7,828 | same winner at step 7,641 |
| Free For All, four fighters | 19,000 | the same three eliminations at steps 7,630, 10,832 and 18,607 |
| One on One, Terrordome (venue 6) | 7,273 | same winner at step 7,127 |

In lockstep the state stream's `rng_util` and the fingerprint's utility part read the
step's private copy (`defjam_lockstep_utility_state`); the real generator is moved by
menus every frame.

Still different between runs, without effect on play so far: the fighters' walk-in
pose floats at step 2 (they converge), and the pad table's live A presses that the test
harness makes. The background crowd's frames agree since v0.14.2 (two online games, one
with HD packs, 18,622 steps).

## Not covered yet

- Two different machines (CPUs, drivers); floating point is the same binary and SSE
  code, so it is expected to agree, but that is unmeasured.
- Inputs that arrive when no step runs (pauses, menus): netplay exchanges inputs per
  frame there.
- A second match in one process (the match generator's additive term is pinned for
  it, but no run has compared one): `two-matches` cannot run on step-timed input,
  because nothing drives the menus after the first match.
- The intermittent start-up stall (the USB deferred routine never runs) is not a
  determinism problem but stops a run.
