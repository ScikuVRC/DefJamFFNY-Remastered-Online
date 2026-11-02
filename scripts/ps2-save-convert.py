#!/usr/bin/env python3
"""Turn PlayStation 2 Def Jam: Fight for NY profiles into Xbox profiles for the port.

    python scripts/ps2-save-convert.py Mcd001.ps2 --list
    python scripts/ps2-save-convert.py Mcd001.ps2 --profile SCI --profile MARC --save-dir "<data folder>/save"
    python scripts/ps2-save-convert.py save.max --out converted

The source is a PCSX2 memory card (.ps2, with or without ECC), an Action Replay MAX save (.max), or a
profile file already copied off a card (a PCSX2 folder card holds them as plain files). Profiles of the
US (BASLUS-21004) and European (BESLES-52507) releases are recognised by their contents. The options
save (DJFFNY::Global) is not converted.

With --save-dir the converted profiles go into the port's save area through save-import.py, which never
replaces a profile already there unless --force (it backs that one up first). With --out they are
written as profile folders (SaveMeta.xbx and the signed profile file) that save-import.py can take later.

Why the files differ. Both games save a profile as a raw copy of the same structure, 0x590C bytes on
the PS2 and 0x5904 on the Xbox (cleared by FUN_001037f8 in SLUS_210.04 and sub_000146D0 in default.xbe).
The fields are the same and in the same order except:
  - the two pictures of the created fighter (a 32x32 head and a 128x128 body, 8-bit with a 256-colour
    palette each) start 6 bytes later on the PS2, which aligns them, and are stored in each console's
    own texture layout: PS2 PSMT8 block order with a CSM1 palette of RGBA (alpha 0x80 = opaque), Xbox
    swizzled (Morton) order with a palette of BGRA (alpha 0xFF = opaque);
  - the PS2 pads the ten single-byte fields after the pictures by 2 bytes.
The file header differs too: the PS2 has a 12-byte checksum where the Xbox has three HMAC-SHA1 fields
(see save-import.py), and the identity line names the release ("DJFFNY::User 17 <build>"), which must
be the Xbox one.

Standard library only.
"""
import argparse
import importlib.util
import struct
import sys
import tempfile
from pathlib import Path

PS2_HEADER = 0x5C            # "20CM", total, 0x40, payload size, 12-byte checksum, 64-byte identity
PS2_PAYLOAD = 0x590C
XBOX_PAYLOAD = 0x5904
IDENTITY_LEN = 0x40
USER_IDENTITY = b'DJFFNY::User 17 '
XBOX_IDENTITY = b'DJFFNY::User 17 -2030886228'

# (PS2 offset, Xbox offset, length) of the payload parts that are copied as they are.
PLAIN = [(0x0000, 0x0000, 0x017E),
         (0x4D84, 0x4D7E, 0x000A),
         (0x4D90, 0x4D88, 0x0B7C)]
# (PS2 offset of the pixels, Xbox offset, size, PS2 offset of the palette, Xbox offset)
PICTURES = [(0x0184, 0x017E, 32, 0x0584, 0x057E),
            (0x0984, 0x097E, 128, 0x4984, 0x497E)]


# ---- PS2 memory card images ---------------------------------------------------------------------
# Superblock, indirect FAT and 512-byte directory entries as documented by mymc and PCSX2.

class Card:
    def __init__(self, raw):
        self.raw = raw
        if raw[:28] != b'Sony PS2 Memory Card Format ':
            raise ValueError('not a PS2 memory card image')
        (self.page_len, self.pages_per_cluster, _, _, self.clusters_per_card,
         self.alloc_offset, _, self.rootdir) = struct.unpack_from('<4H4I', raw, 0x28)
        self.ifc = struct.unpack_from('<32I', raw, 0x50)
        self.raw_page = len(raw) // (self.clusters_per_card * self.pages_per_cluster)   # 528 with ECC
        self.cluster_len = self.page_len * self.pages_per_cluster

    def cluster(self, n):
        pages = []
        for i in range(self.pages_per_cluster):
            o = (n * self.pages_per_cluster + i) * self.raw_page
            pages.append(self.raw[o:o + self.page_len])
        return b''.join(pages)

    def fat(self, n):
        per = self.cluster_len // 4
        indirect = struct.unpack_from('<%dI' % per, self.cluster(self.ifc[(n // per) // per]))
        return struct.unpack_from('<%dI' % per, self.cluster(indirect[(n // per) % per]))[n % per]

    def read(self, first, size):
        out, n = [], first
        while len(out) * self.cluster_len < size and len(out) <= self.clusters_per_card:
            out.append(self.cluster(self.alloc_offset + n))
            entry = self.fat(n)
            if entry == 0xFFFFFFFF or not entry & 0x80000000:
                break
            n = entry & 0x7FFFFFFF
        return b''.join(out)[:size]

    def entries(self, first, count):
        data = self.read(first, count * 512)
        for i in range(count):
            e = data[i * 512:(i + 1) * 512]
            mode, _, length = struct.unpack_from('<HHI', e, 0)
            if mode & 0x8000:          # in use
                yield mode, length, struct.unpack_from('<I', e, 0x10)[0], \
                    e[0x40:0x60].split(b'\0')[0].decode('latin-1')

    def files(self):
        """(folder, name, data) of every file one folder down from the root."""
        _, count, _, _ = next(self.entries(self.rootdir, 1))
        for mode, length, first, folder in self.entries(self.rootdir, count):
            if mode & 0x20 and folder not in ('.', '..'):
                for m2, l2, f2, name in self.entries(first, length):
                    if m2 & 0x10:
                        yield folder, name, self.read(f2, l2)


# ---- Action Replay MAX saves (.max) -------------------------------------------------------------
# "Ps2PowerSave", crc, folder[32], title[32], compressed length (counted from 0x58), file count,
# unpacked length, then LZARI (Okumura: N=4096, F=60, THRESHOLD=2). Unpacked, each file is a u32 size,
# a name[32] and its data, padded so that the next one starts at 8 mod 16.

def lzari_decode(src, size):
    N, F, THRESHOLD = 4096, 60, 2
    Q1, Q2, Q3, Q4 = 1 << 15, 2 << 15, 3 << 15, 4 << 15
    n_char = 256 - THRESHOLD + F
    bits = [0]

    def bit():
        i = bits[0]
        bits[0] += 1
        return (src[i >> 3] >> (7 - (i & 7))) & 1 if i >> 3 < len(src) else 0

    char_to_sym, sym_to_char = [0] * n_char, [0] * (n_char + 1)
    sym_freq, sym_cum, pos_cum = [0] * (n_char + 1), [0] * (n_char + 1), [0] * (N + 1)
    for sym in range(n_char, 0, -1):
        char_to_sym[sym - 1], sym_to_char[sym], sym_freq[sym] = sym, sym - 1, 1
        sym_cum[sym - 1] = sym_cum[sym] + 1
    for i in range(N, 0, -1):
        pos_cum[i - 1] = pos_cum[i] + 10000 // (i + 200)

    def update(sym):
        if sym_cum[0] >= Q1 - 1:
            c = 0
            for i in range(n_char, 0, -1):
                sym_cum[i] = c
                sym_freq[i] = (sym_freq[i] + 1) >> 1
                c += sym_freq[i]
            sym_cum[0] = c
        i = sym
        while sym_freq[i] == sym_freq[i - 1]:
            i -= 1
        if i < sym:
            ci, cs = sym_to_char[i], sym_to_char[sym]
            sym_to_char[i], sym_to_char[sym] = cs, ci
            char_to_sym[ci], char_to_sym[cs] = sym, i
        sym_freq[i] += 1
        while i > 0:
            i -= 1
            sym_cum[i] += 1

    st = {'low': 0, 'high': Q4, 'value': 0}
    for _ in range(17):
        st['value'] = 2 * st['value'] + bit()

    def narrow(cum, k_low, k_high, total):
        low, rng = st['low'], st['high'] - st['low']
        st['high'] = low + rng * cum[k_low] // total
        st['low'] = low + rng * cum[k_high] // total
        low, high, value = st['low'], st['high'], st['value']
        while True:
            if low >= Q2:
                value, low, high = value - Q2, low - Q2, high - Q2
            elif low >= Q1 and high <= Q3:
                value, low, high = value - Q1, low - Q1, high - Q1
            elif high > Q2:
                break
            low, high, value = 2 * low, 2 * high, 2 * value + bit()
        st['low'], st['high'], st['value'] = low, high, value

    def search(cum, x, hi):
        i, j = 1, hi
        while i < j:
            k = (i + j) // 2
            if cum[k] > x:
                i = k + 1
            else:
                j = k
        return i

    text, r, out = bytearray(b' ' * N), N - F, bytearray()
    while len(out) < size:
        x = ((st['value'] - st['low'] + 1) * sym_cum[0] - 1) // (st['high'] - st['low'])
        sym = search(sym_cum, x, n_char)
        narrow(sym_cum, sym - 1, sym, sym_cum[0])
        c = sym_to_char[sym]
        update(sym)
        if c < 256:
            start, length = None, 1
        else:
            x = ((st['value'] - st['low'] + 1) * pos_cum[0] - 1) // (st['high'] - st['low'])
            pos = search(pos_cum, x, N) - 1
            narrow(pos_cum, pos, pos + 1, pos_cum[0])
            start, length = (r - pos - 1) & (N - 1), c - 255 + THRESHOLD
        for k in range(length):         # a byte at a time: a match may overlap what it writes
            ch = c if start is None else text[(start + k) & (N - 1)]
            out.append(ch)
            text[r] = ch
            r = (r + 1) & (N - 1)
    return bytes(out[:size])


def max_files(raw):
    magic, _, folder, _, clen, count, size = struct.unpack_from('<12sL32s32sLLL', raw, 0)
    if magic != b'Ps2PowerSave':
        raise ValueError('not an Action Replay MAX save')
    data, off = lzari_decode(raw[0x5C:0x58 + clen], size), 0
    folder = folder.split(b'\0')[0].decode('latin-1')
    for _ in range(count):
        length, name = struct.unpack_from('<L32s', data, off)
        off += 36
        yield folder, name.split(b'\0')[0].decode('latin-1'), data[off:off + length]
        off = ((off + length + 8 + 15) // 16) * 16 - 8


# ---- The conversion -----------------------------------------------------------------------------

def ps2_profile(blob):
    """The profile's name when blob is a PS2 Def Jam profile file, else None."""
    if len(blob) != PS2_HEADER + PS2_PAYLOAD or blob[:4] != b'20CM':
        return None
    size, header, payload = struct.unpack_from('<III', blob, 4)
    if (size, header, payload) != (len(blob), 0x40, PS2_PAYLOAD):
        return None
    if not blob[0x1C:0x5C].startswith(USER_IDENTITY):
        return None
    return blob[PS2_HEADER:PS2_HEADER + 16].split(b'\0')[0].decode('latin-1')


def ps2_texel(x, y, width):
    """Where pixel (x, y) of a PSMT8 picture is in the PS2 save (the GS block order)."""
    block = (y & ~0xF) * width + (x & ~0xF) * 2
    swap = (((y + 2) >> 2) & 1) * 4
    row = (((y & ~3) >> 1) + (y & 1)) & 7
    return block + row * width * 2 + ((x + swap) & 7) * 4 + ((y >> 1) & 1) + ((x >> 2) & 2)


def xbox_texel(x, y, size):
    """Where pixel (x, y) of a square swizzled picture is in the Xbox save (x bit first)."""
    out, bit, step = 0, 0, 1
    while step < size:
        out |= (1 << bit) if x & step else 0
        out |= (1 << (bit + 1)) if y & step else 0
        bit, step = bit + 2, step << 1
    return out


def csm1(i):
    """The PS2 palette slot of colour i: bits 3 and 4 of the index swapped."""
    return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


def convert_payload(ps2):
    xbox = bytearray(XBOX_PAYLOAD)
    for src, dst, length in PLAIN:
        xbox[dst:dst + length] = ps2[src:src + length]
    for src, dst, size, src_pal, dst_pal in PICTURES:
        for y in range(size):
            for x in range(size):
                xbox[dst + xbox_texel(x, y, size)] = ps2[src + ps2_texel(x, y, size)]
        for i in range(256):
            r, g, b, a = ps2[src_pal + csm1(i) * 4:src_pal + csm1(i) * 4 + 4]
            xbox[dst_pal + i * 4:dst_pal + i * 4 + 4] = bytes((b, g, r, min(255, (a * 255 + 64) // 128)))
    return bytes(xbox)


def convert(blob, save_import):
    """The Xbox profile file (signed with the port key) for a PS2 profile file."""
    payload = convert_payload(blob[PS2_HEADER:])
    head = bytearray(save_import.IDENTITY_END)
    head[0:16] = b'20CM' + struct.pack('<III', save_import.IDENTITY_END + len(payload), 0x40, len(payload))
    head[save_import.HEADER:save_import.IDENTITY_END] = XBOX_IDENTITY.ljust(IDENTITY_LEN, b'\0')
    return save_import.resign(bytes(head) + payload, save_import.PORT_KEY)


def read_source(path):
    """(where, name, PS2 profile file) of every Def Jam profile in the source."""
    raw = path.read_bytes()
    if raw.startswith(b'Sony PS2 Memory Card Format '):
        files = Card(raw).files()
    elif raw.startswith(b'Ps2PowerSave'):
        files = max_files(raw)
    else:
        files = [(path.parent.name, path.name, raw)]
    for folder, name, data in files:
        profile = ps2_profile(data)
        if profile is not None:
            yield '%s/%s' % (folder, name), profile, data


def load_save_import():
    spec = importlib.util.spec_from_file_location('save_import', Path(__file__).with_name('save-import.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source', help='a PS2 memory card (.ps2), an Action Replay MAX save (.max) or a profile file')
    ap.add_argument('--list', action='store_true', help='list the profiles in the source; write nothing')
    ap.add_argument('--profile', action='append', default=[], metavar='NAME',
                    help='convert only this profile (repeat for more); all of them when left out')
    ap.add_argument('--out', help='write the converted profile folders here')
    ap.add_argument('--save-dir', help='import the converted profiles into this save area (holds UserData)')
    ap.add_argument('--force', action='store_true', help='with --save-dir: replace profiles already there')
    args = ap.parse_args(argv)
    say = print

    source = Path(args.source)
    if not source.is_file():
        say('error: %s is not a file' % source)
        return 2
    try:
        found = list(read_source(source))
    except (OSError, ValueError, struct.error, IndexError) as err:
        say('error: cannot read %s: %s' % (source, err))
        return 2
    if not found:
        say('error: no Def Jam: Fight for NY profiles in %s' % source)
        return 1
    if args.list or not (args.out or args.save_dir):
        for where, name, _ in found:
            say('%-12s %s' % (name, where))
        if not args.list:
            say('pass --out or --save-dir to convert them')
        return 0

    wanted = {n.upper() for n in args.profile}
    chosen, seen = [], set()
    for where, name, data in found:
        if wanted and name.upper() not in wanted:
            continue
        if name in seen:
            say('skip   %s (%s): a profile of that name was converted already' % (where, name))
            continue
        seen.add(name)
        chosen.append((where, name, data))
    missing = wanted - {n.upper() for n in seen}
    for name in sorted(missing):
        say('error: no profile named %s in %s' % (name, source))
    if not chosen or missing:
        return 1

    save_import = load_save_import()
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(args.out) if args.out else Path(tmp)
        for where, name, data in chosen:
            if not save_import.NAME_RE.match(name):
                say('skip   %s: %r is not a name the Xbox game can use' % (where, name))
                continue
            folder = out / save_import.folder_name(name)
            folder.mkdir(parents=True, exist_ok=True)
            (folder / 'SaveMeta.xbx').write_bytes(('﻿Name=%s\r\n' % name).encode('utf-16-le'))
            (folder / name).write_bytes(convert(data, save_import))
            say('convert %s (%s) -> %s' % (where, name, folder))
        if args.save_dir:
            return save_import.main([str(out), '--save-dir', args.save_dir] + (['--force'] if args.force else []))
    return 0


if __name__ == '__main__':
    sys.exit(main())
