"""src/hooks/netplay_invite.c: invite codes, made and read."""
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
#include "netplay_invite.h"
/* make <port> <public|-> <lan|->  |  read <text> */
int main(int argc, char **argv)
{
    if (!strcmp(argv[1], "make")) {
        char code[NETPLAY_INVITE_LEN];
        const char *pub = strcmp(argv[3], "-") ? argv[3] : NULL, *lan = strcmp(argv[4], "-") ? argv[4] : NULL;
        if (!netplay_invite_make((unsigned short)atoi(argv[2]), pub, lan, code, sizeof code)) return 3;
        puts(code);
        return 0;
    }
    {
        char out[NETPLAY_INVITE_MAX][32];
        int n = netplay_invite_read(argv[2], out, NETPLAY_INVITE_MAX), i;
        for (i = 0; i < n; i++) puts(out[i]);
        return n ? 0 : 4;
    }
}
'''


@pytest.fixture(scope='module')
def tool(tmp_path_factory):
    folder = tmp_path_factory.mktemp('netplay-invite')
    (folder / 'driver.c').write_text(DRIVER)
    sources = [str(folder / 'driver.c'), str(ROOT / 'src/hooks/netplay_invite.c')]
    if os.name == 'nt':
        cc = shutil.which('cl')
        if not cc:
            pytest.skip('no C compiler')
        exe = folder / 'invite.exe'
        cmd = [cc, '/nologo', '/I' + str(ROOT / 'src/hooks')] + sources + ['/Fe:' + str(exe)]
    else:
        cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        if not cc:
            pytest.skip('no C compiler')
        exe = folder / 'invite'
        cmd = [cc, '-std=c99', '-I' + str(ROOT / 'src/hooks')] + sources + ['-o', str(exe)]
    r = subprocess.run(cmd, cwd=folder, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return exe


def run(tool, *args):
    r = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True)
    return r.returncode, r.stdout.split()


def make(tool, port, public, lan):
    code, out = run(tool, 'make', port, public or '-', lan or '-')
    assert code == 0
    return out[0]


def test_both_addresses_round_trip_home_first(tool):
    code = make(tool, 7777, '203.0.113.45', '192.168.2.20')
    assert code.startswith('DJ-') and len(code.replace('-', '')) == 22
    assert run(tool, 'read', code) == (0, ['192.168.2.20:7777', '203.0.113.45:7777'])


@pytest.mark.parametrize('public,lan,expect', [
    ('203.0.113.45', None, ['203.0.113.45:7777']),
    (None, '10.0.0.5', ['10.0.0.5:7777']),
    ('255.255.255.255', '0.0.0.1', ['0.0.0.1:7777', '255.255.255.255:7777']),
])
def test_one_or_the_other(tool, public, lan, expect):
    assert run(tool, 'read', make(tool, 7777, public, lan)) == (0, expect)


def test_port_is_kept(tool):
    assert run(tool, 'read', make(tool, 65535, '198.51.100.7', None)) == (0, ['198.51.100.7:65535'])


def test_forgiving_input(tool):
    code = make(tool, 7777, '203.0.113.45', '192.168.2.20')
    plain = code.replace('-', '')
    for typed in (code.lower(), ' ' + plain, plain[2:], code.replace('-', ' '),
                  code.replace('0', 'O').replace('1', 'I')):
        assert run(tool, 'read', typed)[1] == ['192.168.2.20:7777', '203.0.113.45:7777'], typed


def test_a_typo_is_caught(tool):
    code = make(tool, 7777, '203.0.113.45', '192.168.2.20')
    letters = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'
    caught = 0
    for i in range(3, len(code)):
        if code[i] == '-':
            continue
        changed = code[:i] + letters[(letters.index(code[i]) + 1) % 32] + code[i + 1:]
        caught += run(tool, 'read', changed)[0] == 4
    assert caught >= len(code.replace('-', '')) - 3     # the CRC catches any one wrong letter but the last bits
    assert run(tool, 'read', code[:-3])[0] == 4           # cut short


@pytest.mark.parametrize('text', ['192.168.2.20:7777', 'host:7777', 'myfriend.example.org', '26.1.2.3', ''])
def test_addresses_are_not_codes(tool, text):
    assert run(tool, 'read', text)[0] == 4


def test_nothing_to_invite_to(tool):
    assert run(tool, 'make', 7777, '-', '-')[0] == 3
    assert run(tool, 'make', 7777, 'not-an-ip', '-')[0] == 3
    assert run(tool, 'make', 0, '203.0.113.45', '-')[0] == 3
