# Def Jam Remastered

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Def Jam: Fight for NY** on the PC, recompiled from the original Xbox game: Story mode, every
match type, sharper rendering, modern controllers, mods, and online matches for up to four
players with your own created fighters.

This is a native port, not an emulator: the Xbox executable is translated to C and compiled
ahead of time, and your computer supplies the graphics, audio and input. You need **your own
USA Xbox copy of the game**. Nothing from the game is in this repository.

![4:3 versus 16:9: the same fight moment with HD textures](docs/media/widescreen-comparison.png)

## What's in it

**Online play**
- Up to four players over a home network or the internet: One on One, Team Match, Free For
  All and the special matches. One player hosts (TCP port 7777); the others join with the
  host's address.
- A lobby where everyone joins and ticks *Ready*, then a fighter select where each player picks
  any fighter or **the created fighter of one of their own profiles**, and locks in.
- Straight into the match: the game skips its logos and menus once the match is agreed.
- After a match everyone votes **Rematch** or **Quit**; a rematch starts only when all agree.
- Each player keeps their own saves, settings and unlocks; the other players' created fighters
  are added for the match and taken out again afterwards.
- Lockstep play: the games exchange inputs every fight step and check each other's fight state
  every second, so both screens play the same fight.

**Saves**
- Your profiles, settings and mods live in your data folder.
- **PS2 profiles carry over**: Story progress, cash, unlocks and your created fighter, from a
  PCSX2 memory card or an Action Replay MAX save
  ([Importing PS2 saves](docs/importing-ps2-saves.md)).
- Profiles from an original Xbox can be brought over too
  ([Importing Xbox saves](docs/importing-xbox-saves.md)).

**Look and feel**
- 60 frames a second, 1×–4× internal resolution, window or full screen.
- Widescreen 16:9 and 16:10, with the HUD and menus kept in shape; original 4:3 still there.
- HD texture packs: HD Fighters, HD Widescreen Menus, or a faithful 4× upscale made from your
  own copy ([HD textures](docs/hd-textures.md)).
- The game's own French menus and English subtitles in every Story cutscene.

**Controls and settings**
- Xbox, PlayStation, Switch Pro and other pads, with remapping, hot-plug and rumble; the
  keyboard can play too ([Controllers, keyboard and mouse](docs/10-input.md)).
- A launcher before the game and an overlay during it (F1): display, controls, mods and more
  ([Launcher and overlay](docs/launcher-and-overlay.md)).
- EA's own developer menu, restored: set up any match, play any Story scene, look around a
  venue ([The developer menu](docs/developer-menu.md)).

**Mods**
- Texture packs, enabled and ordered on the launcher's Mods page.
- File mods: music, speech, models and arenas, laid out like the game folder
  ([Mod manager](docs/mod-manager.md)). Everyone in an online match needs the same file mods.

## Getting started

1. Have your own USA Xbox disc image or extracted copy, matching the
   [supported dump manifest](config/dump-manifest.json). PS2 and GameCube copies cannot be
   played (but their saves can be imported).
2. Build the game from source on Windows: [Build and play](docs/build-and-play.md). It needs
   Visual Studio Build Tools (C++, with CMake and Ninja), Python 3.12 or newer and Git; the
   build turns your copy's executable into the game on your own machine.
3. Start it, choose your display and controller settings in the launcher, and press **Play** —
   or **Online Match** to host or join.

Windows 10 or 11, 64-bit, with a DirectX 11 graphics card. The code also builds on macOS
(Apple Silicon) and Linux ([Build on macOS and Linux](docs/build-macos-linux.md)), but this
project is tested on Windows.

## Online play in short

- **Host:** launcher → *Online Match* → *Host*. The lobby asks your router to open the port
  (UPnP, which most home routers have on) and shows the address to give the others, with a
  Copy button. If your router can't, the lobby says why: then forward TCP port 7777 to your PC
  in the router yourself.
- **Join:** launcher → *Online Match* → enter `host-address:7777`.
- Everyone needs the same build and the same copy of the game; the lobby says if they differ.

How it works, and what is not done yet: [Online play](docs/netplay.md).

## Known limits

- Online play uses TCP: a lost packet stalls both games until it is resent.
- More: [Known issues](docs/known-issues.md).

## Documentation

[docs/README.md](docs/README.md) lists every guide: setup, settings, input, mods, saves,
online play and the developer tools.

## How it works

The [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit translates the Xbox
executable into C, which is compiled ahead of time. The game's code runs on an Xbox
compatibility runtime, with Direct3D 11 (or Vulkan) graphics, audio and input from your
computer. This repository holds the port's code, build tools and documentation only: no disc
image, game data or compiled game.

## License

The code is MIT licensed: see [LICENSE](LICENSE), which keeps the original copyright notice
as the license requires. The toolkit in `tools/xboxrecomp` carries its own MIT license and
includes LGPL-2.1+ components derived from xemu; see its `LICENSE`, `NOTICE` and `LICENSES/`.
The license grants no rights to the game or its artwork.

This is an unofficial fan project, not affiliated with Electronic Arts, AKI or Def Jam.
Def Jam: Fight for NY and its assets belong to their respective owners.
