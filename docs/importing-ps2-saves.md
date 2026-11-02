# Importing PS2 saves

Story profiles from the PlayStation 2 game (a console card dumped to a file, or a PCSX2 memory card)
can be brought into the port. The PS2 and Xbox games save a profile as a copy of the same structure,
so its contents carry over: name, story progress, cash, stats, unlocks and the created fighter. The
file format differs in a few places, which `scripts/ps2-save-convert.py` rewrites; then it signs the
profile the way `scripts/save-import.py` does (see [importing-xbox-saves.md](importing-xbox-saves.md)).

Tested so far (Windows, November 2026): three profiles from a PCSX2 card of the US release, imported
with this tool, were listed on Select User ID with their pictures and story progress, loaded into the
Story crib, and the wardrobe showed each created fighter as the PS2 picture had it. The European
release's profiles have the same layout and convert, but were not loaded in the game. The options save
(volumes and the game-wide unlocks) is not converted. Saves are the game's own data, so they are not
published in this repository.

## Steps

1. **Find the save.** Any of these works:
   - a PCSX2 memory card, `memcards/Mcd001.ps2` and so on (PCSX2's default cards are `Mcd001.ps2` and
     `Mcd002.ps2`; a card named after the game may be a different one);
   - an Action Replay MAX save (`.max`);
   - a profile file copied off a card (in a PCSX2 *folder* memory card they are plain files, for
     example `BASLUS-21004SCI/BASLUS-21004SCI`).
2. **List the profiles in it:**

   ```
   python scripts/ps2-save-convert.py Mcd001.ps2 --list
   ```

   A card can hold two profiles of the same name (from two playthroughs); only the first is converted,
   so convert the other from its own card or file.
3. **Close the game**, then convert and import the ones you want (leave out `--profile` for all):

   ```
   python scripts/ps2-save-convert.py Mcd001.ps2 --profile SCI --profile MARC --save-dir "<data folder>/save"
   ```

   An existing profile of the same name is kept unless you add `--force` (it is backed up first, as
   with `save-import.py`). `--out <folder>` writes the converted profile folders instead, for
   `save-import.py` or for a closer look.
4. **Start the game** and pick the profile on the Select User ID screen.

The game lists profiles in the order of their folder names, so a converted profile can come before
your own in the list.

## What changes

Both games write the profile as a raw copy of one structure, 0x590C bytes on the PS2 and 0x5904 on
the Xbox: compare `FUN_001037f8` in `SLUS_210.04` with `sub_000146D0` in `default.xbe`, which clear
the same fields at offsets that differ only as below.

- **The two pictures** of the created fighter (a 32x32 head and a 128x128 body, 8 bits a pixel with a
  palette of 256 colours each) begin 6 bytes later on the PS2, which aligns them. Each console keeps
  them in its own texture layout: the PS2 in the GS's PSMT8 block order with a CSM1 palette of RGBA,
  where alpha 0x80 is opaque; the Xbox swizzled, with a palette of BGRA, where alpha 0xFF is opaque.
  The tool reorders the pixels and palettes and scales the alpha.
- **Ten one-byte fields** after the pictures (a flag at Xbox `+0x4D8A`, eight bytes at `+0x4D8B`,
  one at `+0x4D93`, object offsets) are followed by 2 bytes of padding on the PS2 only.
- **The header.** The PS2 has a 12-byte checksum where the Xbox has the three HMAC-SHA1 fields, and the
  identity line names the release: `DJFFNY::User 17 -2048007603` on the PS2 and
  `DJFFNY::User 17 -2030886228` on the Xbox. The tool writes the Xbox one.

Everything else is copied byte for byte. `tests/unit/test_ps2_save_convert.py` checks the layout and
the conversion on synthetic data.
