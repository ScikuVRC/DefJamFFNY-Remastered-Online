#!/usr/bin/env python3
"""New fixes in the other port (vyanhursky/DefJamFFNY-recomp and its toolkit fork) since the last review.

D254: we take the other port's fixes one by one, by cherry-pick, never by merging. This lists what
it has added since the commits recorded in config/other-port.json, so nothing is missed and nothing
is read twice.

    python scripts/other-port.py           fetch, then list new commits (game and toolkit)
    python scripts/other-port.py --all     include docs/chore/release commits too
    python scripts/other-port.py mark      record the current heads as reviewed

The game side is the `upstream` remote's main; the toolkit side is the commit that main pins in
tools/xboxrecomp, read from the submodule's `origin` remote (their fork). A commit already
cherry-picked here (its hash in a "cherry picked from commit" line) is marked "taken".
"""
import argparse
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLKIT = os.path.join(REPO, "tools", "xboxrecomp")
STATE = os.path.join(REPO, "config", "other-port.json")
SKIP = re.compile(r"^(docs|chore|ci|release|test|style)(\(|:)", re.I)


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8",
                          check=True).stdout


def taken(cwd, rev):
    """Hashes cherry-picked into `rev`, from the -x trailers."""
    return set(re.findall(r"cherry picked from commit ([0-9a-f]{40})", git(cwd, "log", "--format=%B", rev)))


def new_commits(cwd, base, head, done, show_all):
    rows = []
    for line in git(cwd, "log", "--no-merges", "--reverse", "--format=%H\t%cs\t%s", f"{base}..{head}").splitlines():
        sha, day, subject = line.split("\t", 2)
        if not show_all and SKIP.match(subject):
            continue
        rows.append((sha, day, subject, sha in done))
    return rows


def show(title, rows):
    print(f"{title}: {len(rows)} new" + (" (all taken)" if rows and all(r[3] for r in rows) else ""))
    for sha, day, subject, was_taken in rows:
        print(f"  {sha[:7]} {day} {'[taken] ' if was_taken else ''}{subject}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", nargs="?", default="list", choices=["list", "mark"])
    ap.add_argument("--all", action="store_true", help="include docs/chore/ci/release/test commits")
    ap.add_argument("--no-fetch", action="store_true")
    a = ap.parse_args()

    with open(STATE, encoding="utf-8") as f:
        state = json.load(f)
    if not a.no_fetch:
        git(REPO, "fetch", "-q", "upstream")
        git(TOOLKIT, "fetch", "-q", "origin")
    game_head = git(REPO, "rev-parse", "upstream/main").strip()
    pin = git(REPO, "ls-tree", "upstream/main", "tools/xboxrecomp").split()[2]
    try:
        git(TOOLKIT, "cat-file", "-e", pin + "^{commit}")
    except subprocess.CalledProcessError:
        sys.exit(f"their toolkit pin {pin[:7]} is not in the submodule's remotes; fetch their fork branches")

    if a.action == "mark":
        state["game"], state["toolkit"] = game_head, pin
        with open(STATE, "w", encoding="utf-8", newline="\n") as f:
            json.dump(state, f, indent=2)
            f.write("\n")
        print(f"reviewed: game {game_head[:7]}, toolkit {pin[:7]}")
        return

    show("game (upstream/main " + game_head[:7] + ")",
         new_commits(REPO, state["game"], game_head, taken(REPO, "HEAD"), a.all))
    show("toolkit (their pin " + pin[:7] + ")",
         new_commits(TOOLKIT, state["toolkit"], pin, taken(TOOLKIT, "HEAD"), a.all))
    print("after reviewing: python scripts/other-port.py mark (and commit config/other-port.json)")


if __name__ == "__main__":
    main()
