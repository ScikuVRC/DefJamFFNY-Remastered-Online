"""scripts/ps2-save-convert.py: PS2 profiles to Xbox profiles, on synthetic data only."""
import importlib.util
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


conv = load('ps2_save_convert', 'ps2-save-convert.py')
si = load('save_import', 'save-import.py')


def ps2_profile(name='SCI'):
    """A synthetic PS2 profile file: each payload byte is its offset's low byte, the pictures are
    gradients put in the PS2 block order, and palette entry k is (k, 255 - k, 7, k & 0x7F)."""
    payload = bytearray(i & 0xFF for i in range(conv.PS2_PAYLOAD))
    payload[0:16] = name.encode().ljust(16, b'\0')
    for src, _, size, src_pal, _ in conv.PICTURES:
        for y in range(size):
            for x in range(size):
                payload[src + conv.ps2_texel(x, y, size)] = (x + 3 * y) & 0xFF
        for k in range(256):
            payload[src_pal + k * 4:src_pal + k * 4 + 4] = bytes((k, 255 - k, 7, k & 0x7F))
    head = bytearray(conv.PS2_HEADER)
    head[0:16] = b'20CM' + struct.pack('<III', conv.PS2_HEADER + conv.PS2_PAYLOAD, 0x40, conv.PS2_PAYLOAD)
    head[0x1C:0x5C] = b'DJFFNY::User 17 -2048007603'.ljust(64, b'\0')
    return bytes(head) + bytes(payload)


def test_the_layout_fills_the_xbox_payload_once_and_skips_only_the_ps2_padding():
    xbox, ps2 = [], []
    for src, dst, length in conv.PLAIN:
        xbox.append((dst, dst + length))
        ps2.append((src, src + length))
    for src, dst, size, src_pal, dst_pal in conv.PICTURES:
        xbox += [(dst, dst + size * size), (dst_pal, dst_pal + 0x400)]
        ps2 += [(src, src + size * size), (src_pal, src_pal + 0x400)]
    xbox.sort()
    ps2.sort()
    assert xbox[0][0] == 0 and xbox[-1][1] == conv.XBOX_PAYLOAD
    assert all(a[1] == b[0] for a, b in zip(xbox, xbox[1:]))
    gaps = [(a[1], b[0]) for a, b in zip(ps2, ps2[1:]) if a[1] != b[0]]
    assert gaps == [(0x17E, 0x184), (0x4D8E, 0x4D90)]
    assert ps2[0][0] == 0 and ps2[-1][1] == conv.PS2_PAYLOAD


def test_texel_orders_are_permutations():
    for size in (32, 128):
        cells = range(size * size)
        assert sorted(conv.ps2_texel(x, y, size) for y in range(size) for x in range(size)) == list(cells)
        assert sorted(conv.xbox_texel(x, y, size) for y in range(size) for x in range(size)) == list(cells)
    assert [conv.xbox_texel(x, y, 4) for y in range(2) for x in range(2)] == [0, 1, 2, 3]


def test_csm1_swaps_the_middle_rows_of_each_32_colours():
    assert [conv.csm1(i) for i in (0, 7, 8, 15, 16, 23, 24, 40)] == [0, 7, 16, 23, 8, 15, 24, 48]
    assert all(conv.csm1(conv.csm1(i)) == i for i in range(256))


def test_conversion():
    ps2 = ps2_profile()
    assert conv.ps2_profile(ps2) == 'SCI'
    xbox = conv.convert(ps2, si)
    assert si.header_fault(xbox) is None
    assert all(si.signatures_ok(xbox, si.PORT_KEY))
    assert xbox[si.HEADER:si.IDENTITY_END].rstrip(b'\0') == conv.XBOX_IDENTITY
    payload, source = xbox[si.IDENTITY_END:], ps2[conv.PS2_HEADER:]
    for src, dst, length in conv.PLAIN:
        assert payload[dst:dst + length] == source[src:src + length]
    for _, dst, size, _, dst_pal in conv.PICTURES:
        for y in range(size):
            for x in range(size):
                assert payload[dst + conv.xbox_texel(x, y, size)] == (x + 3 * y) & 0xFF
        for i in range(256):
            k = conv.csm1(i)
            alpha = min(255, ((k & 0x7F) * 255 + 64) // 128)
            assert payload[dst_pal + i * 4:dst_pal + i * 4 + 4] == bytes((7, 255 - k, k, alpha))


def test_only_user_profiles_are_taken():
    ps2 = ps2_profile()
    assert conv.ps2_profile(ps2[:-1]) is None
    options = ps2[:0x1C] + b'DJFFNY::Global 17 -2048007603'.ljust(64, b'\0') + ps2[0x5C:]
    assert conv.ps2_profile(options) is None


def test_main_writes_a_profile_folder(tmp_path):
    source = tmp_path / 'BASLUS-21004MARC'
    source.write_bytes(ps2_profile('MARC'))
    out = tmp_path / 'out'
    assert conv.main([str(source), '--out', str(out)]) == 0
    folder = out / si.folder_name('MARC')
    assert si.parse_save_meta((folder / 'SaveMeta.xbx').read_bytes()) == 'MARC'
    assert si.header_fault((folder / 'MARC').read_bytes()) is None
    assert conv.main([str(source), '--profile', 'NOBODY', '--out', str(out)]) == 1
