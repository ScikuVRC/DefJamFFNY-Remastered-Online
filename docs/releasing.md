# Source releases

A GitHub Release is a versioned source snapshot with game-free setup tools,
checksums and provenance. The setup compiles locally from the player's dump.
The locally built game uses
`win-x64-release` (optimized `RelWithDebInfo`, with diagnostic symbols). Hosted
CI builds runtime libraries and native fixtures with `CMAKE_BUILD_TYPE=Release`.
These three uses of “release” do not imply a downloadable game executable.

## Before tagging

1. Bump `project(defjam_recomp VERSION X.Y.Z)` in `CMakeLists.txt` and write
   `docs/releases/vX.Y.Z.md`: what changed for players, what was tested and how, and the
   known limits. Update [Known issues](known-issues.md) to match.
2. Check the change: `python -m pytest tests/unit` (with `cl` on the path, the native
   fixtures run too), `python scripts/check-source-tree.py`, a release build
   (`scripts/build.ps1 -Preset win-x64-release`) and a play-test of what changed. For a change
   to the runtime, the renderer, the lifter or the toolkit, also `python scripts/regress.py --quick`.
3. Commit to `main` and push. CI on that commit must be green, the three setup builds
   included.

The full regression (`python scripts/regress.py`, about an hour per platform, on a machine
with the game) and a real Setup install plus a Clean Update from the previous version are
worth running before a version that changes what players run; the release notes say which
were done.

## Tagging and publishing

1. Update `project(... VERSION ...)` in CMake only for an actual version change.
   Add `docs/releases/vMAJOR.MINOR.PATCH.md` with supported input, build instructions,
   local gameplay evidence and known limits.
2. Review the source publication audit and run `python scripts/check-source-tree.py`.
   CI checks the current tree; publishing an existing private repository also
   requires auditing all reachable history. Removed files remain in old commits.
3. Do not publish generated code, executables, game
   files, saves or audio. Only the explicitly approved images are allowed; CI
   verifies their exact fingerprints (D58, D98).
4. Create an annotated version tag at that commit on `main` and push the tag.
   The Source release workflow validates the tag against CMake, release notes,
   main ancestry and source policy, then reuses the complete CI workflow.
5. After all checks pass, the workflow creates a draft Release using those notes.
   Verify every asset's SHA-256 against GitHub's digest and its provenance, then
   publish the draft (`gh release edit vX.Y.Z --draft=false --latest`). Version tags are
   immutable; use a new version to correct a published release.

GitHub supplies the source ZIP/tar archives. They do not include submodules;
the supported command-line setup is a recursive clone at the tag. Only the
setup tools (`DefJamSetup-<version>-windows-x64.exe`, `-macos-arm64.dmg`,
`-linux-x64.tar.gz`), each with its `.sha256` and `.provenance.json`, are
attached after standalone setup inventory/tests pass. `check-setup-assets.py`
refuses development payloads and generated/game inputs. Source-tree binary bans
remain intact; a precompiled game is never published. See [setup installer](setup-installer.md)
for behavior and remaining acceptance gates.
The fork URL and exact gitlink make dependency source
and license notices available without game data.

## CI coverage

CI runs project tests on Ubuntu, native save compatibility and toolkit CPU tests
under MSVC, the runtime Release build, and separate Release kernel bridge,
directory, USB and vertex-program fixtures. WARP exercises D3D11 without a physical
GPU. Each native project has registered tests; empty CTest suites fail.

`scripts/test-toolkit-msvc.py` selects `cl` for the pinned toolkit's shared
compiled-fixture helper, which otherwise prefers an installed Clang. It leaves
test sources, assertions and `/O2` unchanged and validates a reviewed helper
source fingerprint before replacement. Other compiler-dependent tests retain their own gates. This checks
Windows/MSVC; it does not establish Clang portability.

Game regression and captures require a verified user dump on a local machine.
They remain separate from hosted CI and must precede a gameplay-affecting release.
See [Contributing](../CONTRIBUTING.md) and the [test plan](02-test-plan.md).

## Mod packs

The HD packs are not part of a version release. They and their signed catalogue are assets
of the separate `mods` release (a pre-release, never marked latest, so the launcher's
**Download Setup** link and Setup's own release list skip it). `scripts/publish-mods.py`
publishes them; see [Mod management](mod-manager.md#publishing-a-pack-or-a-new-version).
