"""Compile the launcher's quick starts (src/hooks/launcher_start.c) and check the pad scripts
they hand the game: START and A to the main menu, the menu's choice once its music starts, and
DOWN to the profile on the Select User ID list, counted from MemCard.GetNumUsers()."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
HOLD = 0.3 if os.name == 'nt' else 0.2

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher_start.h"
int main(int argc, char **argv)
{
    char out[1400];
    if (!strcmp(argv[1], "script")) {
        size_t n = launcher_start_script(atoi(argv[2]), atoi(argv[3]), out, sizeof out);
        printf("%u|%s\n", (unsigned)n, out);
    } else if (!strcmp(argv[1], "title")) {
        launcher_title_presses(out, sizeof out);
        printf("%s\n", out);
    } else if (!strcmp(argv[1], "start")) {
        printf("%d\n", launcher_start_target());
        launcher_start(atoi(argv[2]), atoi(argv[3]));
        printf("%d\n%s\n%s\n%s\n%s\n", launcher_start_target(), getenv("RECOMP_SCRIPT_ANCHOR") ? getenv("RECOMP_SCRIPT_ANCHOR") : "-",
               getenv("RECOMP_PAD_SCRIPT") ? getenv("RECOMP_PAD_SCRIPT") : "-",
               getenv("RECOMP_PAD_SCRIPT_FORMAT") ? getenv("RECOMP_PAD_SCRIPT_FORMAT") : "-",
               getenv("RECOMP_PAD_HOST") ? getenv("RECOMP_PAD_HOST") : "-");
    }
    return 0;
}
'''

TITLE, MENU, STORY, BATTLE = range(4)


@pytest.fixture(scope='module')
def tool(tmp_path_factory):
    compiler = shutil.which('cl') if os.name == 'nt' else shutil.which('cc')
    if not compiler:
        pytest.skip('the quick-start fixture needs a C compiler')
    folder = tmp_path_factory.mktemp('launcher_start')
    driver = folder / 'driver.c'
    driver.write_text(DRIVER)
    exe = folder / ('start.exe' if os.name == 'nt' else 'start')
    sources = [driver, ROOT / 'src/hooks/launcher_start.c']
    if os.name == 'nt':
        cmd = [compiler, '/nologo', '/TC', '/I' + str(ROOT / 'src/hooks'), *map(str, sources), '/Fe:' + str(exe)]
    else:
        cmd = [compiler, '-I' + str(ROOT / 'src/hooks'), *map(str, sources), '-o', str(exe)]
    result = subprocess.run(cmd, cwd=folder, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr

    def run(*args, env=None):
        e = {k: v for k, v in os.environ.items() if not k.startswith('RECOMP_')}
        e.update(env or {})
        return subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, env=e,
                              check=True).stdout.splitlines()
    return run


def script(tool, target, user):
    n, text = tool('script', target, user)[0].split('|', 1)
    assert int(n) == len(text)
    return text


def presses(entries):
    return ','.join(entries)


def test_title_presses_alternate_start_and_a_until_the_main_menu(tool):
    title = tool('title')[0].split(',')
    assert title[:3] == ['start:1:1.3', 'a:2:2.3', 'start:3:3.3']
    assert len(title) == 41 and title[39] == 'a:40:40.3'
    assert title[-1] == '@getscreeninfo(intmain/mainmenu'


def test_title_start_has_no_script_and_the_main_menu_only_the_title_presses(tool):
    assert script(tool, TITLE, 0) == ''
    assert script(tool, MENU, 3) == tool('title')[0]


def test_story_goes_down_past_create_user_id_to_the_profile(tool):
    title = tool('title')[0]
    # Profile 2 is the third below CREATE USER ID: three presses, then A.
    downs = ['down:%g:%g' % (1.5 + .6 * i, 1.5 + .6 * i + HOLD) for i in range(3)]
    assert script(tool, STORY, 2) == title + ',' + presses(
        ['@audio.playmusic(', 'a:3:3.3', '@memcard.getnumusers(', *downs, 'a:3.7:4'])


def test_battle_moves_right_and_leaves_the_user_ids_to_its_own_screen(tool):
    title = tool('title')[0]
    battle = title + ',' + presses(['@audio.playmusic(', 'right:3:%g' % (3 + HOLD), 'a:6:6.3'])
    # Battle ID asks GetNumUsers as well: presses waiting for it would land there.
    assert script(tool, BATTLE, 1) == battle
    assert script(tool, BATTLE, -1) == battle


def test_no_profile_stops_at_the_user_id_list(tool):
    assert 'getnumusers' not in script(tool, STORY, -1)


def test_start_sets_the_environment_and_keeps_a_tests_own_script(tool):
    target, after, anchor, pad, fmt, host = tool('start', STORY, 0)
    assert (target, after) == ('0', '2')
    assert anchor == 'getscreeninfo(intmain/prestart'
    assert pad == script(tool, STORY, 0)
    assert (fmt, host) == ('seconds', '1')
    out = tool('start', BATTLE, 1, env={'RECOMP_PAD_SCRIPT': 'a:1:2'})
    assert out[1] == '3' and out[3] == 'a:1:2' and out[2] == '-'
