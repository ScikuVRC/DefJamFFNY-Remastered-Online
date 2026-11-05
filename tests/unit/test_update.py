"""setup/update.py: the portable copy's updater, against a fake update channel (no network)."""
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import zipfile

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('defjam_update', ROOT / 'setup/update.py')
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


def signed(manifest, key):
    payload = json.dumps(manifest, sort_keys=True).encode()
    return json.dumps({'payload': base64.b64encode(payload).decode(),
                       'signature': base64.b64encode(key.sign(payload)).decode()}).encode()


def package(files):
    data = io.BytesIO()
    with zipfile.ZipFile(data, 'w') as bundle:
        for name, content in files.items():
            bundle.writestr(name, content)
    return data.getvalue()


def manifest_for(zip_bytes, version='0.16.0'):
    return dict(schema=1, game=update.GAME, version=version, issued='2026-11-05T00:00:00Z', notes='Faster crowds',
                package=dict(url='https://example.invalid/defjam.zip', size=len(zip_bytes),
                             sha256=hashlib.sha256(zip_bytes).hexdigest()))


@pytest.fixture
def channel(tmp_path, monkeypatch):
    """A copy of the game, a key it trusts, and a fake channel serving update.json and the zip."""
    key = Ed25519PrivateKey.generate()
    copy = tmp_path / 'Def Jam Remastered'
    (copy / 'game').mkdir(parents=True)
    (copy / 'game' / 'default.xbe').write_bytes(b'dump')
    (copy / 'data' / 'save').mkdir(parents=True)
    (copy / 'data' / 'save' / 'SCI').write_bytes(b'profile')
    (copy / 'defjam_recomp.exe').write_bytes(b'old exe')
    (copy / 'docs').mkdir()
    (copy / 'docs' / 'netplay.md').write_text('old doc')
    served = {}

    class Response(io.BytesIO):
        def __enter__(self): return self
        def __exit__(self, *a): self.close()

    def open_url(url, headers=None):
        return Response(served[url])

    monkeypatch.setattr(update, 'open_url', open_url)
    monkeypatch.setattr(update, 'trusted_keys', lambda: [key.public_key().public_bytes_raw().hex()])
    return dict(key=key, copy=copy, served=served)


def run(channel, command):
    return update.main([command, '--install-dir', str(channel['copy']), '--data-dir', str(channel['copy'] / 'data'),
                        '--runtime', '0.15.0', '--url', 'https://example.invalid/update.json'])


def view(channel):
    text = (channel['copy'] / 'data' / 'updater' / 'view.ini').read_text()
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)


def test_share_links_become_direct_downloads():
    assert update.direct_url('https://www.dropbox.com/s/abc/update.json?dl=0') == \
        'https://www.dropbox.com/s/abc/update.json?dl=1'
    assert update.direct_url('https://www.dropbox.com/scl/fi/x/update.json?rlkey=k&dl=0') == \
        'https://www.dropbox.com/scl/fi/x/update.json?rlkey=k&dl=1'
    assert update.direct_url('https://drive.google.com/file/d/1AbC_d-9/view?usp=sharing') == \
        'https://drive.google.com/uc?export=download&id=1AbC_d-9'
    assert update.direct_url('https://files.example.org/update.json') == 'https://files.example.org/update.json'
    with pytest.raises(update.UpdateError):
        update.direct_url('http://insecure.example.org/update.json')


def test_check_reports_a_newer_version(channel):
    zip_bytes = package({'defjam_recomp.exe': b'new exe'})
    channel['served']['https://example.invalid/update.json'] = signed(manifest_for(zip_bytes), channel['key'])
    assert run(channel, 'check') == 0
    assert view(channel)['state'] == 'available' and view(channel)['version'] == '0.16.0'
    channel['served']['https://example.invalid/update.json'] = signed(manifest_for(zip_bytes, '0.15.0'), channel['key'])
    assert run(channel, 'check') == 0 and view(channel)['state'] == 'current'


def test_tampered_or_foreign_signatures_change_nothing(channel):
    zip_bytes = package({'defjam_recomp.exe': b'new exe'})
    good = json.loads(signed(manifest_for(zip_bytes), channel['key']))
    payload = json.loads(base64.b64decode(good['payload']))
    payload['package']['url'] = 'https://evil.invalid/x.zip'
    good['payload'] = base64.b64encode(json.dumps(payload, sort_keys=True).encode()).decode()
    channel['served']['https://example.invalid/update.json'] = json.dumps(good).encode()
    assert run(channel, 'install') == 1 and 'not signed' in view(channel)['message']
    channel['served']['https://example.invalid/update.json'] = signed(manifest_for(zip_bytes), Ed25519PrivateKey.generate())
    assert run(channel, 'install') == 1
    assert (channel['copy'] / 'defjam_recomp.exe').read_bytes() == b'old exe'


def test_install_replaces_the_game_files_and_keeps_the_rest(channel):
    zip_bytes = package({'defjam_recomp.exe': b'new exe', 'defjam_recomp.pdb': b'symbols',
                         'docs/netplay.md': 'new doc', 'setup/update.py': '# new updater'})
    channel['served']['https://example.invalid/update.json'] = signed(manifest_for(zip_bytes), channel['key'])
    channel['served']['https://example.invalid/defjam.zip'] = zip_bytes
    assert run(channel, 'install') == 0
    copy = channel['copy']
    assert (copy / 'defjam_recomp.exe').read_bytes() == b'new exe'
    assert (copy / 'defjam_recomp.exe.old').read_bytes() == b'old exe'      # renamed aside, as a running one is
    assert (copy / 'docs' / 'netplay.md').read_text() == 'new doc'
    assert (copy / 'data' / 'save' / 'SCI').read_bytes() == b'profile'
    assert (copy / 'game' / 'default.xbe').read_bytes() == b'dump'
    assert view(channel)['state'] == 'installed'
    assert not any(p.name.startswith('update-') for p in copy.iterdir())   # staging removed
    # The next check removes what was renamed aside.
    assert run(channel, 'check') == 0 and not (copy / 'defjam_recomp.exe.old').exists()


def test_a_corrupt_download_changes_nothing(channel):
    zip_bytes = package({'defjam_recomp.exe': b'new exe'})
    channel['served']['https://example.invalid/update.json'] = signed(manifest_for(zip_bytes), channel['key'])
    channel['served']['https://example.invalid/defjam.zip'] = zip_bytes[:20] + bytes([zip_bytes[20] ^ 0xFF]) + zip_bytes[21:]
    assert run(channel, 'install') == 1 and 'does not match' in view(channel)['message']
    assert (channel['copy'] / 'defjam_recomp.exe').read_bytes() == b'old exe'


@pytest.mark.parametrize('name', ['game/default.xbe', 'data/settings.ini', '../evil.exe', 'other.exe',
                                  'data/save/SCI', 'docs/../../x'])
def test_packages_may_only_hold_the_games_own_files(tmp_path, name):
    archive = tmp_path / 'p.zip'
    archive.write_bytes(package({'defjam_recomp.exe': b'x', name: b'y'}))
    with pytest.raises(update.UpdateError):
        update.entries(archive)


def test_a_failed_move_puts_everything_back(tmp_path, monkeypatch):
    copy = tmp_path / 'copy'
    copy.mkdir()
    (copy / 'defjam_recomp.exe').write_bytes(b'old exe')
    (copy / 'defjam_recomp.pdb').write_bytes(b'old pdb')
    archive = tmp_path / 'p.zip'
    archive.write_bytes(package({'defjam_recomp.exe': b'new exe', 'defjam_recomp.pdb': b'new pdb'}))
    staging = tmp_path / 'staging'
    real = update.os.replace
    calls = []

    def failing(src, dst):
        calls.append(dst)
        if str(dst).endswith('defjam_recomp.pdb') and 'new' in str(src):
            raise OSError('disk full')
        return real(src, dst)

    monkeypatch.setattr(update.os, 'replace', failing)
    with pytest.raises(update.UpdateError, match='previous version was kept'):
        update.install_files(archive, update.entries(archive), copy, staging)
    assert (copy / 'defjam_recomp.exe').read_bytes() == b'old exe'
    assert (copy / 'defjam_recomp.pdb').read_bytes() == b'old pdb'


def test_publish_script_signs_what_the_updater_accepts(tmp_path):
    spec = importlib.util.spec_from_file_location('publish_update', ROOT / 'scripts/publish-update.py')
    publish = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(publish)
    assert publish.version().count('.') == 2
    zip_path = tmp_path / 'pkg.zip'
    publish.build_zip({'defjam_recomp.exe': Path(__file__)}, zip_path)
    assert update.entries(zip_path) == ['defjam_recomp.exe']
    key = tmp_path / 'other.key'
    from cryptography.hazmat.primitives import serialization as s
    key.write_text(Ed25519PrivateKey.generate().private_bytes(s.Encoding.Raw, s.PrivateFormat.Raw, s.NoEncryption()).hex())
    with pytest.raises(SystemExit, match='not in config/mods/catalogue-keys.json'):
        publish.sign({'x': 1}, key)
