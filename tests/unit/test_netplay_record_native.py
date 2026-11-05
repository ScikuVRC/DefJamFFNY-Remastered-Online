"""src/hooks/netplay_record.c: the player's online record (ID, history, stats, rivals, replays kept)."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netplay_record.h"

/* add <W|L|D|U> <when> <venue> <fighter> <profile|-> <replay|-> [<opponent> <id>]...
 * id | stats | rivals | history | prune <dir> <keep> | parse <line> */
int main(int argc, char **argv)
{
    static OnlineMatchRecord h[ONLINE_HISTORY_MAX];
    if (!strcmp(argv[1], "id")) { puts(online_player_id()); return 0; }
    if (!strcmp(argv[1], "add")) {
        OnlineMatchRecord r;
        int i;
        memset(&r, 0, sizeof r);
        r.result = argv[2][0];
        r.when = atoll(argv[3]);
        r.venue = atoi(argv[4]);
        r.fighter = atoi(argv[5]);
        if (strcmp(argv[6], "-")) snprintf(r.profile, sizeof r.profile, "%s", argv[6]);
        if (strcmp(argv[7], "-")) snprintf(r.replay, sizeof r.replay, "%s", argv[7]);
        snprintf(r.name, sizeof r.name, "Me");
        for (i = 8; i + 1 < argc && r.opponents < ONLINE_OPPONENTS; i += 2) {
            snprintf(r.opponent[r.opponents], sizeof r.opponent[0], "%s", argv[i]);
            snprintf(r.opponent_id[r.opponents], sizeof r.opponent_id[0], "%s", argv[i + 1]);
            r.opponents++;
        }
        r.counted = r.result != 'U' && r.opponents > 0;
        return online_record_add(&r) ? 0 : 3;
    }
    if (!strcmp(argv[1], "history")) {
        int n = online_history(h, ONLINE_HISTORY_MAX), i;
        for (i = 0; i < n; i++)
            printf("%c %lld %d %s %s %d %s\n", h[i].result, h[i].when, h[i].counted, h[i].profile[0] ? h[i].profile : "-",
                   h[i].replay[0] ? h[i].replay : "-", h[i].opponents, h[i].opponents ? h[i].opponent_id[0] : "-");
        return 0;
    }
    if (!strcmp(argv[1], "stats")) {
        OnlineStats s;
        int n = online_history(h, ONLINE_HISTORY_MAX);
        online_stats(h, n, &s);
        printf("matches=%d wins=%d losses=%d draws=%d streak=%d best=%d favourite=%d profile=%s fav_matches=%d venue=%d vw=%d vl=%d title=%s\n",
               s.matches, s.wins, s.losses, s.draws, s.streak, s.best_streak, s.favourite,
               s.favourite_profile[0] ? s.favourite_profile : "-", s.favourite_matches, s.best_venue, s.best_venue_wins,
               s.best_venue_losses, online_title(s.wins));
        return 0;
    }
    if (!strcmp(argv[1], "rivals")) {
        OnlineRival r[16];
        int n = online_history(h, ONLINE_HISTORY_MAX), k = online_rivals(h, n, r, 16), i;
        for (i = 0; i < k; i++)
            printf("%s %s %d %d %d %d %d %lld\n", r[i].id, r[i].name, r[i].matches, r[i].wins, r[i].losses, r[i].draws,
                   r[i].best_streak, r[i].last);
        return 0;
    }
    if (!strcmp(argv[1], "prune")) { online_prune_replays(argv[2], atoi(argv[3])); return 0; }
    if (!strcmp(argv[1], "parse")) {
        OnlineMatchRecord r;
        if (!online_record_parse(argv[2], &r)) return 4;
        printf("%c %d %d %s\n", r.result, r.counted, r.opponents, r.replay[0] ? r.replay : "-");
        return 0;
    }
    return 2;
}
'''

A = '0123456789abcdef'
B = 'fedcba9876543210'


@pytest.fixture(scope='module')
def tool(tmp_path_factory):
    folder = tmp_path_factory.mktemp('netplay-record')
    (folder / 'driver.c').write_text(DRIVER)
    sources = [str(folder / 'driver.c'), str(ROOT / 'src/hooks/netplay_record.c')]
    if os.name == 'nt':
        cc = shutil.which('cl')
        if not cc:
            pytest.skip('no C compiler')
        exe = folder / 'record.exe'
        cmd = [cc, '/nologo', '/I' + str(ROOT / 'src/hooks')] + sources + ['/Fe:' + str(exe)]
    else:
        cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        if not cc:
            pytest.skip('no C compiler')
        exe = folder / 'record'
        cmd = [cc, '-std=c99', '-D_DEFAULT_SOURCE', '-I' + str(ROOT / 'src/hooks')] + sources + ['-o', str(exe)]
    r = subprocess.run(cmd, cwd=folder, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return exe


@pytest.fixture
def run(tool, tmp_path):
    env = dict(os.environ, DEFJAM_DATA=str(tmp_path))

    def call(*args, ok=0):
        r = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True, env=env)
        assert r.returncode == ok, r.stdout + r.stderr
        return r.stdout.splitlines()
    call.data = tmp_path
    return call


def fields(line):
    return dict(kv.split('=') for kv in line.split())


def test_the_id_is_made_once_and_kept(run):
    first = run('id')[0]
    assert len(first) == 16 and all(c in A for c in first)
    assert run('id')[0] == first
    assert (run.data / 'online' / 'player.ini').read_text().strip() == 'id=' + first


def test_history_is_newest_first_and_survives_odd_names(run):
    run('add', 'W', 100, 1, 99, 'MY FIGHTER', '20261105-1.djr', 'Friend', A)
    run('add', 'L', 200, 2, 56, '-', '-', 'Mike', B)
    lines = run('history')
    assert lines == ['L 200 1 - - 1 ' + B, 'W 100 1 MY FIGHTER 20261105-1.djr 1 ' + A]


def test_stats_streaks_favourite_and_venue(run):
    # oldest first: W W L W W W at venue 3 (and one at 4), a draw last but one, then an unfinished
    seq = ['W', 'W', 'L', 'W', 'W', 'D', 'W', 'U']
    for i, result in enumerate(seq):
        venue = 4 if i == 1 else 3
        run('add', result, 1000 + i, venue, 99 if i < 5 else 56, 'SCI' if i < 5 else '-', '-', 'Friend', A)
    s = fields(run('stats')[0])
    assert (s['matches'], s['wins'], s['losses'], s['draws']) == ('7', '5', '1', '1')
    assert s['streak'] == '1'                   # the newest counted is a win after a draw
    assert s['best'] == '2'
    assert (s['favourite'], s['profile'], s['fav_matches']) == ('99', 'SCI', '5')
    assert (s['venue'], s['vw'], s['vl']) == ('3', '4', '1')
    assert s['title'] == 'Contender'


def test_practice_against_the_cpu_is_listed_but_not_counted(run):
    run('add', 'W', 1, 1, 56, '-', '-')
    assert run('history') == ['W 1 0 - - 0 -']
    assert fields(run('stats')[0])['matches'] == '0'


def test_rivals_by_id_keep_the_latest_name(run):
    run('add', 'W', 1, 1, 56, '-', '-', 'Old', A)
    run('add', 'W', 2, 1, 56, '-', '-', 'New', A)
    run('add', 'L', 3, 1, 56, '-', '-', 'New', A)
    run('add', 'W', 4, 1, 56, '-', '-', 'Mike', B)
    run('add', 'U', 5, 1, 56, '-', '-', 'Mike', B)
    lines = run('rivals')
    assert lines[0] == A + ' New 3 2 1 0 2 3'
    assert lines[1] == B + ' Mike 1 1 0 0 1 5'


def test_parse_rejects_paths_and_bad_lines(run):
    good = '1\t5\tW\t1\t0\t1\t56\t-\tMe\tx.djr\t1\tFriend\t' + A
    assert run('parse', good) == ['W 1 1 x.djr']
    assert run('parse', good.replace('x.djr', '..\\evil.djr'), ok=4) == []
    assert run('parse', good.replace('\tW\t', '\tQ\t'), ok=4) == []
    assert run('parse', '2' + good[1:], ok=4) == []
    # a bad ID is dropped, and a match without opponents is never counted
    assert run('parse', good.replace(A, 'not-an-id')) == ['W 1 1 x.djr']
    assert run('parse', '1\t5\tW\t1\t0\t1\t56\t-\tMe\t-\t0') == ['W 0 0 -']


def test_prune_keeps_the_newest(run, tmp_path):
    folder = tmp_path / 'replays'
    folder.mkdir()
    for name in ['20261101-000000-1.djr', '20261102-000000-1.djr', '20261103-000000-1.djr', 'notes.txt']:
        (folder / name).write_text('x')
    run('prune', folder, 2)
    assert sorted(p.name for p in folder.iterdir()) == ['20261102-000000-1.djr', '20261103-000000-1.djr', 'notes.txt']
