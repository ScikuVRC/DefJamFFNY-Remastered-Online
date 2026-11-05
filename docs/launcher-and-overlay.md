# Launcher and overlay

The launcher and in-game overlay edit the same settings and save as you change them. Both work with a
mouse, keyboard or gamepad on Windows, macOS and Linux, including the Steam Deck.

## Home and Advanced

The launcher opens before the game loads. Home fills the window like a console's home screen: the
city drifting behind everything, the logo, a row of tiles along the bottom, and above them, large,
what the lit tile does, with the profile's own created fighter beside it for Continue and Profiles.

| Tile | What it does |
|---|---|
| **Continue story** | Starts the game and goes straight to the profile's crib: the intro is skipped, and the title screen, the main menu and the Select User ID list are pressed through for you. Shows the profile's face, Story %, Story time, cash and when it was last saved, read from the save. With no profile it is **New story** and stops on Select User ID, where you create one. |
| **Battle** | Straight to Choose Match Type. Each player picks a user ID after it, on the game's own screen. |
| **Online** | Host or join an online match ([Online play](netplay.md)). |
| **Main menu** | The game's own main menu, without the intro. |
| **Profiles** | Choose whose story Continue picks up (`[launcher] profile`; empty means the most recently saved). |
| **Mods** | The texture and file mod page. |

The pad stays live while those screens are pressed through, and takes over where the presses end.
Left and right move along the tiles, A or Enter takes one, START opens the settings and B or Escape
asks before quitting; **Controls**, **Settings** and **Quit** are also at the top right. Startup-only
settings apply to this launch when the game starts.

The art is optional and comes from your own copy: `cover_city` and `cover_logo` (`.png` or `.jpg`) in
`<data folder>/launcher`. Without them Home draws dark stripes and a text logo. The profiles' pictures
and figures are read from the saves. Nothing from the game is in this repository.

![The compact Home, for small windows and large text](media/launcher.png)

In a window under about 760 by 470 points (more at large text sizes) Home is the compact one above:
a **Play** button, which starts the game as before, and the common choices: display mode, window size,
picture shape, render scale, VSync and local HD textures, with **Configure controls**.

**Settings** (Advanced) opens all settings in the same window. **Mods** opens its texture-pack page
directly. The pages share Home's look: its city dimmed behind them, amber for what is chosen.
**< Home**, Escape or the pad's B button returns to Home, except while capturing a
binding or using a popup. **Open Logs**, **Open Folder** and **Update / Repair** are on the compact Home.
Enlarged text/narrow windows use a compact title and a category selector. The body scrolls; footer
actions remain available. Short windows with large text use a compact footer; the skip preference
remains in Interface. Display DPI and the existing text/control size setting still apply.

- **Skip:** select *Skip launcher next time* in the footer or Interface (`[launcher] skip`).
- **Bring it back:** hold Shift at startup or pass `--launcher`; `--no-launcher` skips it once.
  On macOS, hold Shift while opening the app, or use `open "Def Jam Recompiled.app" --args --launcher`.
- Test runs (`RECOMP_SETTINGS=none`) and headless runs (`RECOMP_HEADLESS=1`) do not show the launcher.
- On macOS the Metal launcher closes before the game window opens. Its red close button/Cmd+Q quits.


## Updates

A portable copy of the game (a folder started with `Play.bat`) updates itself from a private
channel of your own: no build tools, no Setup. Whoever builds the game runs
`scripts/publish-update.py`, which packs the program, its symbols, the docs and the helper
scripts into `defjam-<version>.zip` and writes `update.json` (version, size, SHA-256, signed
with the mod catalogue's key). Both go wherever you like that gives a direct link (Dropbox,
Google Drive: share links are turned into direct downloads). Every copy's `settings.ini` has the
link to `update.json` as `[update] url`, or Settings > Maintenance > Updates > Update link;
upload each new `update.json` over the old one so the link stays the same.

When the launcher opens it checks that link (`setup/update.py` with the copy's own Python).
A newer version puts **Update to vX** at the top of Home and in Maintenance; it downloads the
zip, checks its signature and SHA-256, and replaces only the game's own files (never `game/`
or `data/`). The running program is renamed `defjam_recomp.exe.old` (Windows allows that) and
removed at the next start; if any step fails, every file moved is put back. **Restart to
update** then starts the new version.

## The settings pages

![Advanced Display settings with brighter slider tracks and contextual help](media/launcher-advanced.png)

| Page | Contents |
|---|---|
| Display | Fullscreen, window dimensions, picture shape, filter, VSync, render scale; widescreen HUD/cutscene/side-fill/FOV options; brightness, game brightness curve and vignette |
| Controls | Players/devices and live stick/button feedback, rumble test, keyboard player, startup controller count/backend, dead zones, thresholds, rumble and pointer behavior; expand Gamepad button mapping or Keyboard and mouse bindings for Detect/Add/Clear/Default |
| Mods | Installed local/project packs, enablement and overlap winners; shared Load Order window; verified project catalogue downloads/updates, rollback and removal; Refresh local mods, Open Mods Folder, Memory and manual pack list |
| Interface | Launcher skip, startup movies, overlay key/pad chord, text/control size |
| Diagnostics | Recorder/capture/retention settings, texture authoring, save-affecting unlock tools and, in-game, Story scenes and EA's developer menu |
| Maintenance | Data/save/mod/install/settings/play-log paths with Open and Copy path; setup selection, reset settings, version and licenses |

New settings default to **Windowed, 1920×1080, Widescreen (Auto), 2× render scale**, with VSync on. Existing saved choices are preserved. Auto follows the monitor's shape, so a 16:10 monitor can show bars inside a 16:9 window. The Render scale tooltip explains internal heights: 2× is 960p, 3× is 1440p; window size is separate.

Home, Advanced and the overlay share the same controls and `settings.ini`. Environment overrides show
the effective value, name the variable and disable editing for that run. On a wide window an explanation
pane follows the focused or hovered setting. Existing INI names and saved values are retained.

### Texture packs

Mods lists selected packs first, including missing selections, then other local folders. The origin badge
distinguishes manually installed packs from catalogue-managed downloads. Refresh local mods finds new
folders without enabling them. Unchecking keeps files. **Load Order...** shows all enabled local and
project packs together: highest priority at the top. Higher/Lower moves a pack;
the overlap readout names which final pack wins the same texture ID. Changes apply on the next launch.
Place HD Widescreen Menus above the optional Recomp HD baseline in the priority window. The advanced pack-list field and
`RECOMP_TEXTURE_PACKS` environment locks remain available; overly long lists are rejected visibly.

The **Project catalogue** uses a separate Python helper for network operations. Refresh verifies the
signed catalogue; Install and Update verify archive size/hash and layout, then switch complete pack
folders with recoverable rollback. Installed updates retain the same stable folder and load order.
Installation does not enable a pack. Updates are your choice. A missing/expired catalogue never blocks
offline Play; expired metadata cannot authorize new installs. Incompatible or revoked versions cannot
be newly installed. **Restore previous version** retains one intact managed version. Disable a mod
before **Remove download**; local folders and edited managed files are never silently overwritten.

Catalogue mutation is launcher-only. During a helper job Play is disabled; Cancel keeps the prior
installation unless its short final commit has already completed. An OS-held lock also protects games
started without the launcher. An interrupted commit is recovered before Play; if recovery cannot safely
recognize files, it preserves them and offers Retry recovery. A saved skip-launcher preference is
bypassed for recovery without changing the preference. Old installations without the helper keep local
mod controls and Play. The signed project catalogue provides compatible reviewed downloads.
For authoring, see [Texture Modding — Getting Started](texture-modding-getting-started.md);
for helper behavior, see [mod management](mod-manager.md).

### Folders, logs and setup

Open Folder offers Data, Saves, Mods, Installation and Settings file. Open Logs uses the installed
wrapper's play-log location; a developer checkout uses `logs`. Maintenance shows the resolved paths and
has Copy path, useful in Steam Gaming Mode when a file manager cannot open.

Update / Repair opens a dialog. Choose a locally available **Def Jam Setup** application, or use
**Download Setup** to visit the stable release page. On macOS select `Def Jam Setup.app`, on Windows the
setup `.exe`, on Linux the extracted `DefJamSetup.sh`; first open/extract a dmg/archive. Its location is
remembered in `launcher-setup-path.txt` in the data folder. `DEFJAM_SETUP_PATH` supplies it for one run.
This is only a tool path; gameplay settings remain in `settings.ini`.

**Open Setup** destroys the launcher window, opens the setup wizard and exits the
game process. Setup does not inherit the play lock. No installation starts automatically: choose the
release and operation in the wizard. New installed wrappers supply installation/data/log context, so
setup is prefilled. In the overlay, Maintenance directs you to close the game and return to the launcher
before updating. A missing or moved setup can be selected again.

### Developer options

Diagnostics contains **Unlock all fighters and venues** (`[debug] unlock_all`, off by default).
It runs the developers' unlock routine on screen changes, giving every profile all 74 fighters, every
venue and 10,000 reward points. It changes the profile in memory and asks the game to save it: keep it
off for a save you care about. It applies on Play/next launch. `RECOMP_UNLOCK_ALL` overrides it for a run.
See [unlocking everything](research/unlock-everything.md).

Interface contains **Skip startup movies and legal screen**, preserving `[debug] skip_intro` and its
default off. It starts at Press Start after resource loading; Story scenes and idle attract playback
remain. `RECOMP_SKIP_INTRO` overrides it for a run. See [startup skipping](research/startup-skip-setting.md).

## In-game overlay

Press **F1**, the pad's guide button, or hold both stick clicks for half a second. The same inputs close
it; Esc/B also closes it when no popup or binding capture is open. The game sees no pad/keyboard input
while the overlay is open, but **keeps running**; it does not pause. On a Mac keyboard use fn+F1 if F1 is
the brightness key, or change the shortcut in Interface. The pointer shows while the overlay is open.

Fullscreen, window size, 4:3/stretch, filter, VSync, brightness and vignette apply live. Widescreen,
render scale, brightness curve and other startup settings apply at the next launch; the footer lists
pending changes. Settings in the launcher say **on Play** instead. Widescreen transitions into or out of
a wide canvas are tracked separately from the two live picture shapes.

## Keyboard and gamepad

D-pad/left stick moves, A activates, B goes back/closes, shoulders switch Advanced categories, triggers
adjust sliders faster. Navigation uses the first pad to press a button, independently of the game's own
mapping. Esc cancels binding capture. Returning Home restores focus to its originating utility action.
On Home, START opens the settings and B asks before quitting.

## Implementation and test hooks

Dear ImGui 1.92.9b is fetched at its pinned hash (MIT, `thirdparty/imgui-LICENSE.txt`). Shared pages,
settings controls and overlay are in `src/hooks/pc_ui.cpp`; Home is `pc_launcher_home.cpp`, the profiles
it shows `pc_launcher_profiles.cpp`, and the tiles' pressed-through starts `launcher_start.c`, which
an online match's start shares; palette/fonts and procedural geometry are `pc_ui_theme.cpp`, paths/setup
handoff are `pc_ui_actions.cpp`, and the renderer-independent inventory/timing helpers are
`pc_ui_model.h`. No new artwork or font binary is used.

| | Windows | macOS/Linux |
|---|---|---|
| Launcher | Win32/D3D11 (`pc_launcher.cpp`) | SDL3 renderer, Metal on macOS (`pc_launcher_sdl.cpp`), first-thread event loop |
| Overlay | D3D11 presentation hook | Vulkan/MoltenVK presentation hook |
| Input | Window messages | SDL events from `src/host_posix.c` |

Off-by-default test hooks:

| Variable | Behavior |
|---|---|
| `RECOMP_WINDOW_SHOT=<file.bmp>` | Read back the launcher or overlay window to a 32-bit BMP; macOS/Linux |
| `RECOMP_WINDOW_SHOT_FRAME` | Overlay frame to capture, default 20 |
| `RECOMP_LAUNCHER_SCRIPT` | `<ms>:click:<x>:<y>`, `move`, `resize:<width>:<height>`, `shot:<file>`, `ui:<target>`, `uiopt:<target>` (as `ui`, silent if absent), `play`, `quit`, separated by semicolons; positions in window points |
| `RECOMP_HOST_SCRIPT` | Game window: `<ms>:key:<SDL name>`, `click`, `move`, `text`; times from opening |
| `RECOMP_INPUT_VIRTUAL_PADS`, `RECOMP_INPUT_VIRTUAL_CHORD_MS` | SDL virtual pad tests, including overlay stick-click/B paths ([input guide](10-input.md)) |

Named UI targets include Advanced, Mods, Configure, Back, all six categories, Play, Quit, SetupOpen,
Home's tiles (Continue, Battle, OnlineTile, MainMenu, Profiles, Mods), `Profile:<name>` in the picker, and
setting keys such as `display.brightness` or `debug.unlock_all`, plus `pack.<folder>` and
`pack.earlier.<folder>` / `pack.later.<folder>`. Targets scroll into view, then receive
real SDL press/release events. A missing target is reported; tests do not change a setting directly.
Captures remain local and are never committed.
