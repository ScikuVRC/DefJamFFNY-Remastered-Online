# Mod management

Making your first pack? See [Texture Modding — Getting Started](texture-modding-getting-started.md).
This page explains the manager and its implementation details.

The Mods page has installed local/project packs, a curated project catalogue and
one shared Load Order window. **Highest priority is shown at the top**: use Higher/Lower
to choose which pack overrides matching textures. In the underlying semicolon-separated
INI list, later entries win; the launcher handles that conversion. Downloaded packs are
separate from locally generated Recomp HD (Faithful HD components). Give HD Widescreen
Menus higher priority than that optional baseline. Installing a pack does not enable it.

The launcher starts `setup/mods.py` with the installed version's retained Python
(`python/` beside the source folder in a Setup install, or inside the game folder in a
portable copy started with `Play.bat`). It passes argv directly, polls a bounded atomic INI projection and stays responsive.
The game executable itself makes no network requests. Developer checkouts may set
`DEFJAM_MOD_PYTHON` to a Python with Pillow and cryptography; broken installed Python
must be repaired rather than silently using an unrelated system interpreter.

```sh
python setup/mods.py status --data-dir /path/to/data --runtime 0.10.6
python setup/mods.py refresh --data-dir /path/to/data --runtime 0.10.6
python setup/mods.py install --id example-pack --data-dir /path/to/data --runtime 0.10.6
python setup/mods.py rollback --id example-pack --data-dir /path/to/data
python setup/mods.py remove --id example-pack --data-dir /path/to/data
```

Testing builds offer an explicit "Show testing versions (release candidates)" checkbox.
It affects catalogue choices and installs for this launcher session; stable-only
is the default. Turning it off does not remove an installed testing version.
The CLI equivalent is `--prerelease` for status, refresh and install.

`--capability wide-v1` is appropriate only for a runtime with the wide-layer loader.
The launcher derives that capability from the toolkit compiled into the game.
Released builds with the wide-layer loader advertise it, allowing compatible wide
menu downloads to be offered. Builds from 0.12.4 also pass `--capability language-v1`:
translations are [language mods](language-packs.md), a folder with `language.ini` and
binary deltas (`.djd`), optionally with a `manifest.ini` and PNGs for HD versions of the
images they translate (checked like a texture pack's; the version then also declares
`textures-v1`). They install into the same mods
folder, are ticked on the same page (one language at a time, saved as
`[language] pack`), and are not in the Load Order: they change the game's own data
beneath every texture mod. Other game data/code/recipe mods are outside this
installer. General schema-2 overrides are future work. Full storefront/Quiver adapters are also future work; see the
[storefront assessment](research/mod-storefront-integration-2026-10-21.md).

State lives at `<data>/mod-manager/`, outside runtime pack discovery:

- `catalogue.json`: signed exact bytes, signature, highest accepted revision/hash.
  The public key is pinned in `config/mods/catalogue-keys.json`, not fetched from
  the same site as the index. Empty/missing trust fails closed for fresh installs.
- `state.json`: managed receipts, stable folders, exact installed-file hashes and
  one previous-version receipt. Local matching folder names never establish ownership.
- `downloads/`: size/hash-addressed `.part` and verified ZIP cache. Resume accepts
  correct byte ranges; a server ignoring Range restarts the file, never appends it.
- `staging/` and `rollback/`: private same-filesystem complete folders. Journaled
  renames and state replacement recover process interruption. POSIX directory fsync
  and Windows write-through moves order commit durability; power-loss semantics
  still depend on the filesystem/hardware.
- `writer.lock`: OS-held single helper writer; `runtime.lock`: exclusive mutation,
  shared lifetime game use, including launch without UI. Stale filenames are harmless.
- `journal.json`: interrupted commit marker. Never delete this to bypass a recovery
  error without inspecting the preserved target/rollback files and receipts.
- `view.ini` / `progress.ini`: bounded atomic UI projections, never precedence authority.

The helper rejects unsafe archive paths, links, special/encrypted entries, case
collisions, unknown extensions, excessive expansion, unsupported manifest grammar,
missing/invalid referenced PNG/DDS files, incompatible capabilities and modified
managed/rollback folders. Runtime validation still owns comparison of replacement
scale/layout against a source texture seen in the game. Never treat a validator
pass as proof that a visual mapping is correct.

The catalogue is `catalogue.json` (and its signature, `catalogue.json.sig`) on the
[`mods` release](https://github.com/ScikuVRC/DefJamFFNY-recomp/releases/tag/mods) of this
repository, beside the pack zips it lists. Its records are kept in
`config/mods/catalogue/<id>.json`; the zips and the signing key are not in the repository.

## File mods

A pack can also replace the game's own files, not only its textures. Put them in the
pack's `files` folder, laid out like the game folder, and list the pack in `[mods] packs`
in `settings.ini` (semicolons between names; later packs win; restart the game):

```text
<data>/mods/my-music/files/music/dirty/main.mus      music
<data>/mods/my-arena/files/assets/v2bg.viv           models, arenas (.viv archives)
<data>/mods/my-voices/files/speech/Announcer/...     speech
```

Every file the game opens from its own folder is looked up in the packs first, and the
log says which one was used (`[MODS] music\dirty\main.mus from ...`). Nothing in the game
folder is changed; removing the pack from the list restores the original.

- **Music.** The game asks for each track as loose files first (`music\<track>\tune.mpf`,
  `main.mus`, `loading0.mus`, `overlay0.mus`; the tracks are names such as `dirty` and
  `cnnyessir`) and plays them from `audio\music*.viv` when they are absent. So a track
  can be replaced by putting its files at those paths. They must be EA's MPF/MUS
  format, which the game decodes itself; an MP3 or WAV renamed is not played.
- **Models and arenas.** Fighters, venues and their textures are in the `.viv`
  archives under `assets` and `motion`. A replacement archive has to keep the
  names the game looks for inside it.
- **Data.** `fighter.xml`, `venue.xml`, `gamedata.xml` and the other files at the top
  of the game folder can be replaced the same way.

Online, every player needs the same file packs: a pack that changes models or game
data plays a different fight and the games part (netplay compares the program and
`default.xbe` only).

## Publishing a pack or a new version

`scripts/publish-mods.py` does it, with the GitHub CLI (`gh`, signed in to an account that
can write releases) and Python's `cryptography` module. The zips go in a folder of your
choice (`--store` or `DEFJAM_MOD_STORE`); the private signing key is a file that stays off
git (`--key` or `DEFJAM_CATALOGUE_KEY`). Its public half is in
`config/mods/catalogue-keys.json`; a launcher only trusts catalogues signed with it.

```powershell
$env:DEFJAM_MOD_STORE = 'D:\DefJam\mod-releases'
$env:DEFJAM_CATALOGUE_KEY = 'D:\DefJam\keys\mod-catalogue.ed25519'
# A new version of a pack, from its folder (manifest.ini and its files):
python scripts/publish-mods.py add hd-fighters 1.1.0 --folder D:\DefJam\packs\hd-fighters
# A new pack also needs --name and --description; --min-recomp and --load-after are optional.
python scripts/publish-mods.py publish --dry-run
python scripts/publish-mods.py publish
```

`add` zips the folder (deterministically, under one top folder), unpacks it exactly as the
launcher will to check it installs, and appends the version to the pack's record with its
size, SHA-256 and download address. Commit the changed record. `publish` uploads the zips the
release does not have yet, then signs a new catalogue revision and uploads it. Players see
**Update available** in Mods after **Refresh catalogue**; installed versions are never
replaced without that click, and **Restore previous version** brings back the one before.

Each catalogue revision is good for five years; republish (`publish` alone) before then,
or Refresh and new installs stop working (installed packs keep playing). Revisions only go
up; a launcher refuses an older one than it has seen. Never edit or replace a published
version's zip: publish a new version instead. Losing the signing key means a new key and a
game update that trusts it.
