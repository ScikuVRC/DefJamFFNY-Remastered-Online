"""The project catalogue: its records, and scripts/publish-mods.py with a synthetic pack (no network)."""
import argparse
import importlib.util
import io
import json
from pathlib import Path

import pytest
from PIL import Image
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('publish_mods', ROOT / 'scripts/publish-mods.py')
publish = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publish)


def test_catalogue_records_validate_and_download_from_the_release():
    rows = publish.contract.load_mods(publish.RECORDS)
    assert {row['id'] for row in rows} >= {'hd-fighters', 'menu-ui-hd-wide'}
    for row in rows:
        for version in row['versions']:
            name = publish.asset_name(row['id'], version['version'])
            assert version['archive']['urls'] == [publish.DOWNLOAD + name]
    keys = json.loads((ROOT / 'config/mods/catalogue-keys.json').read_text())['keys']
    assert len(keys) == 1 and len(bytes.fromhex(keys[0])) == 32
    assert publish.mods.CATALOGUE_URL == publish.DOWNLOAD + 'catalogue.json'
    # Copies that cached the old catalogue (revision 11) refuse anything lower.
    assert publish.FIRST_REVISION > 11


def pack_folder(folder):
    image = io.BytesIO()
    Image.new('RGBA', (8, 8), 'red').save(image, format='PNG')
    (folder / 'textures').mkdir(parents=True)
    (folder / 'textures/test.png').write_bytes(image.getvalue())
    (folder / 'manifest.ini').write_text(f'[pack]\nschema=1\nname=Test\n[textures]\n{"a" * 64}=textures/test.png\n')
    (folder / 'README.txt').write_text('Synthetic pack\n')


def test_added_folder_installs_through_the_launcher_helper(tmp_path, monkeypatch):
    monkeypatch.setattr(publish, 'RECORDS', tmp_path / 'records')
    pack_folder(tmp_path / 'pack')
    args = argparse.Namespace(id='paper-test', version='1.0.0', store=str(tmp_path / 'store'),
                              folder=str(tmp_path / 'pack'), zip=None, name='Paper test',
                              description='Synthetic fixture', min_recomp='0.14.0', load_after=None)
    publish.add(args)
    with pytest.raises(SystemExit, match='already'):
        publish.add(args)
    zip_path = tmp_path / 'store/paper-test-1.0.0.zip'
    first = zip_path.read_bytes()
    record = json.loads((tmp_path / 'records/paper-test.json').read_text())
    version = record['versions'][0]
    assert version['archive']['root'] == 'paper-test-v1-0-0' and version['archive']['file_count'] == 3
    assert version['capabilities'] == ['textures-v1']

    # A second version inherits the first one's settings; zips are deterministic.
    (tmp_path / 'pack/README.txt').write_text('Synthetic pack, revised\n')
    publish.add(argparse.Namespace(**dict(vars(args), version='1.1.0', name=None, description=None, min_recomp=None)))
    record = json.loads((tmp_path / 'records/paper-test.json').read_text())
    assert [v['version'] for v in record['versions']] == ['1.0.0', '1.1.0']
    assert record['versions'][1]['min_recomp'] == '0.14.0'
    zip_path.unlink()
    publish.zip_folder(tmp_path / 'pack', 'paper-test-v1-0-0', zip_path)
    assert zip_path.read_bytes() != first  # README changed
    (tmp_path / 'pack/README.txt').write_text('Synthetic pack\n')
    zip_path.unlink()
    publish.zip_folder(tmp_path / 'pack', 'paper-test-v1-0-0', zip_path)
    assert zip_path.read_bytes() == first

    # Signed like publish does, the catalogue is trusted and the newest version installs.
    key = Ed25519PrivateKey.generate()
    rows = publish.contract.load_mods(tmp_path / 'records')
    raw = publish.contract.build(rows, publish.FIRST_REVISION, '2026-01-01T00:00:00Z', '2099-01-01T00:00:00Z')
    public = key.public_key().public_bytes_raw().hex()
    publish.mods.verify_index(raw, key.sign(raw), [public], highest=11, fresh=True)
    manager = publish.mods.Manager(tmp_path / 'data', '0.14.0', keys=[public])
    publish.mods.atomic_json(manager.root / 'catalogue.json', dict(
        raw=publish.mods.base64.b64encode(raw).decode(), signature=publish.mods.base64.b64encode(key.sign(raw)).decode(),
        revision=publish.FIRST_REVISION, sha256=publish.hashlib.sha256(raw).hexdigest()))
    manager.install('paper-test', archive_path=tmp_path / 'store/paper-test-1.1.0.zip')
    assert (tmp_path / 'data/mods/paper-test/README.txt').read_text() == 'Synthetic pack, revised\n'
    assert manager.state()['installed']['paper-test']['version'] == '1.1.0'


def test_folder_with_a_disallowed_file_is_refused(tmp_path):
    pack_folder(tmp_path / 'pack')
    (tmp_path / 'pack/run.exe').write_bytes(b'MZ')
    with pytest.raises(SystemExit, match='Not allowed'):
        publish.zip_folder(tmp_path / 'pack', 'x', tmp_path / 'out.zip')


def test_signing_needs_the_trusted_key(tmp_path):
    key = tmp_path / 'other.key'
    from cryptography.hazmat.primitives import serialization as s
    key.write_text(Ed25519PrivateKey.generate().private_bytes(s.Encoding.Raw, s.PrivateFormat.Raw, s.NoEncryption()).hex())
    with pytest.raises(SystemExit, match='not in config/mods/catalogue-keys.json'):
        publish.sign(b'{}', key)
