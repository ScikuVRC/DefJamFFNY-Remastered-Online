"""src/hooks/netplay_saves.c: another player's profile under another name, checked against
scripts/save-import.py (the signatures, the folder name and SaveMeta.xbx). Synthetic profiles only."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('save_import', ROOT / 'scripts/save-import.py')
save_import = importlib.util.module_from_spec(spec)
spec.loader.exec_module(save_import)

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netplay_saves.h"
/* driver dir <name> | rename <in> <out> <name> | meta <name> <out> | other <name> <k> */
int main(int argc, char **argv)
{
    if (!strcmp(argv[1], "dir")) { char out[13]; netplay_save_dir_name(argv[2], out); puts(out); return 0; }
    if (!strcmp(argv[1], "other")) { char out[64]; netplay_other_name(argv[2], atoi(argv[3]), out, sizeof out); puts(out); return 0; }
    if (!strcmp(argv[1], "meta")) {
        unsigned char buf[160]; size_t n = netplay_save_meta(argv[2], buf, sizeof buf);
        FILE *f = fopen(argv[3], "wb"); fwrite(buf, 1, n, f); fclose(f); return n == 0;
    }
    if (!strcmp(argv[1], "rename")) {
        static unsigned char buf[1 << 20]; size_t n; int ok;
        FILE *f = fopen(argv[2], "rb"); n = fread(buf, 1, sizeof buf, f); fclose(f);
        ok = netplay_rename_profile(buf, n, argv[4]);
        f = fopen(argv[3], "wb"); fwrite(buf, 1, n, f); fclose(f);
        return ok ? 0 : 3;
    }
    return 2;
}
'''


@pytest.fixture(scope='module')
def tool(tmp_path_factory):
    folder = tmp_path_factory.mktemp('netplay-saves')
    (folder / 'driver.c').write_text(DRIVER)
    sources = [str(folder / 'driver.c'), str(ROOT / 'src/hooks/netplay_saves.c')]
    if os.name == 'nt':
        cc = shutil.which('cl')
        if not cc:
            pytest.skip('no C compiler')
        exe = folder / 'saves.exe'
        cmd = [cc, '/nologo', '/I' + str(ROOT / 'src/hooks')] + sources + ['/Fe:' + str(exe)]
    else:
        cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
        if not cc:
            pytest.skip('no C compiler')
        exe = folder / 'saves'
        cmd = [cc, '-std=c99', '-I' + str(ROOT / 'src/hooks')] + sources + ['-o', str(exe)]
    r = subprocess.run(cmd, cwd=folder, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return exe, folder


def run(tool, *args):
    r = subprocess.run([str(tool[0]), *map(str, args)], capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def profile(name, payload_size=0x600):
    payload = bytearray(payload_size)
    payload[0:len(name)] = name.encode()
    payload[0x14:0x18] = struct.pack('<I', 123456)          # cash: a value to keep
    head = bytearray(save_import.IDENTITY_END)
    head[0:16] = b'20CM' + struct.pack('<III', save_import.IDENTITY_END + payload_size, 0x40, payload_size)
    head[save_import.HEADER:save_import.IDENTITY_END] = b'identity'.ljust(64, b'\x07')
    return save_import.resign(bytes(head) + bytes(payload), save_import.PORT_KEY)


@pytest.mark.parametrize('name', ['MARC', 'SCI', 'QU1', 'A', 'LONGER NAME 12'])
def test_folder_name_matches_the_game_hash(tool, name):
    assert run(tool, 'dir', name) == (0, save_import.folder_name(name))


def test_renamed_profile_is_signed_and_keeps_everything_else(tool):
    original = profile('MARC')
    source, target = tool[1] / 'MARC', tool[1] / 'MARC2'
    source.write_bytes(original)
    assert run(tool, 'rename', source, target, 'MARC2')[0] == 0
    renamed = target.read_bytes()
    assert save_import.header_fault(renamed) is None
    assert all(save_import.signatures_ok(renamed, save_import.PORT_KEY))
    assert renamed[0x8C:0x8C + 20] == b'MARC2'.ljust(20, b'\0')
    # Only the name and the signatures change.
    changed = [i for i in range(len(original)) if original[i] != renamed[i]]
    assert all(0x10 <= i < 0x4C or 0x8C <= i < 0x8C + 20 for i in changed)


def test_not_a_profile_or_a_name_too_long_is_refused(tool):
    bad = tool[1] / 'bad'
    bad.write_bytes(b'XXXX' + bytes(0x700))
    assert run(tool, 'rename', bad, tool[1] / 'out', 'MARC2')[0] == 3
    good = tool[1] / 'good'
    good.write_bytes(profile('MARC'))
    assert run(tool, 'rename', good, tool[1] / 'out', 'X' * 20)[0] == 3


def test_save_meta_names_the_save(tool):
    out = tool[1] / 'SaveMeta.xbx'
    assert run(tool, 'meta', 'MARC2', out)[0] == 0
    assert save_import.parse_save_meta(out.read_bytes()) == 'MARC2'
    assert out.read_bytes()[:2] == b'\xff\xfe'


@pytest.mark.parametrize('name,k,expect', [('MARC', 2, 'MARC2'), ('MARC', 10, 'MARC10'),
                                           ('ABCDEFGHIJKLMNO', 2, 'ABCDEFGHIJKLMN2'),
                                           ('ABCDEFGHIJKLMNO', 12, 'ABCDEFGHIJKLM12')])
def test_other_names(tool, name, k, expect):
    assert run(tool, 'other', name, k) == (0, expect)
