#!/usr/bin/env python3
"""Publish mod packs to the project catalogue that the launcher's Mods page installs and updates from.

The catalogue and the pack zips are assets of the "mods" release of this repository. The catalogue
records live in config/mods/catalogue/<id>.json; the zips and the signing key stay outside git.

  add      record a new pack version from a pack folder or a finished zip (writes the zip to --store)
  publish  upload missing zips, then sign and upload a new catalogue revision

Needs the GitHub CLI (gh, signed in to an account that can write releases) and the cryptography module.
"""
import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPO = 'ScikuVRC/DefJamFFNY-recomp'
TAG = 'mods'
DOWNLOAD = f'https://github.com/{REPO}/releases/download/{TAG}/'
RECORDS = ROOT / 'config/mods/catalogue'
# Above the old catalogue's last revision (11), which existing copies still have cached: the
# launcher refuses a catalogue whose revision is lower than the one it saw last.
FIRST_REVISION = 12
LIFETIME = dt.timedelta(days=5 * 365)
LICENCE = ('Derived game artwork; rights remain with their respective owners. '
           'The project source MIT licence does not cover this artwork.')


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    sys.modules[name] = loaded
    spec.loader.exec_module(loaded)
    return loaded


sys.path.insert(0, str(ROOT / 'setup'))
contract = module('mod_catalogue', ROOT / 'setup/mod_catalogue.py')
mods = module('mods', ROOT / 'setup/mods.py')


def sha(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def asset_name(mod_id, version):
    return f'{mod_id}-{version}.zip'


def zip_folder(folder, root, out):
    """Deterministic zip: sorted entries, fixed timestamps; PNG/DDS stored, text compressed."""
    files = sorted(p for p in folder.rglob('*') if p.is_file())
    for path in files:
        if path.is_symlink() or path.suffix.lower() not in mods.ALLOWED:
            raise SystemExit(f'Not allowed in a pack: {path}')
    with zipfile.ZipFile(out, 'x') as bundle:
        for path in files:
            name = root + '/' + path.relative_to(folder).as_posix()
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            stored = path.suffix.lower() in ('.png', '.dds')
            info.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
            with open(path, 'rb') as source, bundle.open(info, 'w') as target:
                shutil.copyfileobj(source, target, 1 << 20)


def describe(archive):
    with zipfile.ZipFile(archive) as bundle:
        rows = [r for r in bundle.infolist() if not r.is_dir()]
        roots = {PurePosixPath(r.filename).parts[0] for r in bundle.infolist()}
    if len(roots) != 1:
        raise SystemExit('The zip must hold exactly one top folder')
    return dict(urls=[], size=archive.stat().st_size, unpacked_size=sum(r.file_size for r in rows),
                file_count=len(rows), sha256=sha(archive), root=roots.pop())


def check_installs(archive, descriptor):
    """Unpack exactly as the launcher does and return the pack's capabilities."""
    with tempfile.TemporaryDirectory(dir=archive.parent) as scratch:
        receipt = mods.unpack_archive(archive, descriptor, Path(scratch) / 'pack')
    return sorted(receipt['capabilities'])


def add(args):
    contract.slug(args.id)
    contract.version(args.version)
    store = Path(args.store)
    store.mkdir(parents=True, exist_ok=True)
    target = store / asset_name(args.id, args.version)
    if target.exists():
        raise SystemExit(f'{target} already exists')
    if args.folder:
        root = args.id + '-v' + args.version.replace('.', '-').lower()
        zip_folder(Path(args.folder), root, target)
    else:
        shutil.copyfile(args.zip, target)
    descriptor = describe(target)
    capabilities = check_installs(target, descriptor)
    descriptor['urls'] = [DOWNLOAD + target.name]

    record_path = RECORDS / (args.id + '.json')
    if record_path.exists():
        record = json.loads(record_path.read_text(encoding='utf-8'))
        if any(v['version'] == args.version for v in record['versions']):
            raise SystemExit(f'{args.id} {args.version} is already recorded')
        previous = max(record['versions'], key=lambda v: mods.semver(v['version']))
    else:
        if not args.name or not args.description:
            raise SystemExit('A new pack needs --name and --description')
        record = dict(id=args.id, name=args.name, description=args.description, authors=['Def Jam Remastered'],
                      licence=LICENCE, homepage=f'https://github.com/{REPO}', revoked_versions=[], versions=[],
                      **{'class': 'linked'})
        previous = dict(load_after=[], depends=[], conflicts=[], min_recomp=args.min_recomp or '0.14.0')
    if args.name:
        record['name'] = args.name
    if args.description:
        record['description'] = args.description
    parsed = contract.version(args.version)
    record['versions'].append(dict(
        version=args.version, channel='prerelease' if parsed[4] else 'stable',
        min_recomp=args.min_recomp or previous['min_recomp'], capabilities=capabilities,
        reviewed=dt.date.today().isoformat(), depends=previous['depends'], conflicts=previous['conflicts'],
        load_after=args.load_after.split(',') if args.load_after else previous['load_after'],
        archive=descriptor))
    contract.validate_mod(record)
    RECORDS.mkdir(parents=True, exist_ok=True)
    record_path.write_text(json.dumps(record, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(f'Recorded {args.id} {args.version}: {target} ({descriptor["size"]:,} bytes); run publish to upload')


def gh(*arguments, capture=True):
    result = subprocess.run(['gh', *arguments, '-R', REPO], capture_output=capture, text=True)
    if result.returncode:
        raise SystemExit(f'gh {" ".join(arguments)} failed: {result.stderr.strip()}')
    return result.stdout


def published_revision():
    try:
        with urllib.request.urlopen(DOWNLOAD + 'catalogue.json', timeout=30) as response:
            return json.loads(response.read())['revision']
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise


def sign(raw, key_path):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    key = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(Path(key_path).read_text().strip()))
    public = key.public_key().public_bytes_raw().hex()
    trusted = json.loads((ROOT / 'config/mods/catalogue-keys.json').read_text())['keys']
    if public not in trusted:
        raise SystemExit('This key is not in config/mods/catalogue-keys.json; the launcher would reject it')
    return key.sign(raw)


def publish(args):
    rows = contract.load_mods(RECORDS)
    store = Path(args.store)
    have = {a['name'] for a in json.loads(gh('release', 'view', TAG, '--json', 'assets') if release_exists() else '{"assets":[]}')['assets']}
    uploads = []
    for row in rows:
        for version in row['versions']:
            url = version['archive']['urls'][0]
            if not url.startswith(DOWNLOAD) or url[len(DOWNLOAD):] in have:
                continue
            local = store / url[len(DOWNLOAD):]
            if not local.is_file() or sha(local) != version['archive']['sha256']:
                raise SystemExit(f'{local} is missing or differs from its record')
            uploads.append(local)
    previous = published_revision()
    revision = FIRST_REVISION if previous is None else previous + 1
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0, tzinfo=None)
    raw = contract.build(rows, revision, now.strftime('%Y-%m-%dT%H:%M:%SZ'), (now + LIFETIME).strftime('%Y-%m-%dT%H:%M:%SZ'))
    signature = sign(raw, args.key)
    keys = json.loads((ROOT / 'config/mods/catalogue-keys.json').read_text())['keys']
    mods.verify_index(raw, signature, keys, fresh=True)
    print(f'Catalogue revision {revision}: {len(rows)} packs; {len(uploads)} zip(s) to upload')
    if args.dry_run:
        return
    if not release_exists():
        gh('release', 'create', TAG, '--prerelease', '--latest=false', '--title', 'Mod packs',
           '--notes', 'The catalogue and packs that the launcher\'s Mods page installs and updates from. '
           'Use launcher > Mods > Refresh catalogue rather than downloading these by hand.')
    for path in uploads:
        print('Uploading', path.name)
        gh('release', 'upload', TAG, str(path), capture=False)
    with tempfile.TemporaryDirectory() as scratch:
        index, sig = Path(scratch) / 'catalogue.json', Path(scratch) / 'catalogue.json.sig'
        index.write_bytes(raw)
        sig.write_bytes(signature)
        gh('release', 'upload', TAG, str(index), str(sig), '--clobber')
    print('Published', DOWNLOAD + 'catalogue.json')


def release_exists():
    return subprocess.run(['gh', 'release', 'view', TAG, '-R', REPO], capture_output=True).returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--store', default=os.environ.get('DEFJAM_MOD_STORE'),
                        help='folder for the pack zips (default: DEFJAM_MOD_STORE)')
    sub = parser.add_subparsers(dest='command', required=True)
    one = sub.add_parser('add')
    one.add_argument('id')
    one.add_argument('version')
    source = one.add_mutually_exclusive_group(required=True)
    source.add_argument('--folder', help='pack folder (manifest.ini and its files)')
    source.add_argument('--zip', help='finished pack zip with one top folder')
    one.add_argument('--name')
    one.add_argument('--description')
    one.add_argument('--min-recomp', help='oldest game version the pack works with')
    one.add_argument('--load-after', help='comma-separated pack IDs this one loads after')
    two = sub.add_parser('publish')
    two.add_argument('--key', default=os.environ.get('DEFJAM_CATALOGUE_KEY'),
                     help='signing key file (default: DEFJAM_CATALOGUE_KEY)')
    two.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    if not args.store:
        parser.error('--store (or DEFJAM_MOD_STORE) is required')
    if args.command == 'publish' and not args.key:
        parser.error('--key (or DEFJAM_CATALOGUE_KEY) is required')
    add(args) if args.command == 'add' else publish(args)


if __name__ == '__main__':
    main()
