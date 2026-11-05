#!/usr/bin/env python3
"""Make an update for portable copies of the game (setup/update.py): a package zip and a signed
update.json, to upload to a private place of your choice.

    python scripts/publish-update.py --url "https://www.dropbox.com/s/.../defjam-0.16.0.zip?dl=0" \
        --key D:\\DefJam\\keys\\mod-catalogue.ed25519 --out D:\\DefJam\\updates [--docs-like "R:\\Def Jam Remastered\\docs"]

1. Run it once without --url: it writes the package zip (defjam-<version>.zip) and says so.
2. Upload the zip; copy its share link.
3. Run it again with --url <that link>: it writes update.json, signed.
4. Upload update.json over the previous one, so its link stays the same. That link is what
   every copy's settings.ini has as [update] url.

The package holds the game's own files: the program and its symbols from the release build,
setup/update.py, setup/mods.py, setup/mod_catalogue.py, config/mods/catalogue-keys.json, and
with --docs-like the repository's version of each guide that folder has. Never game files,
saves or settings. The key is the mod catalogue's (config/mods/catalogue-keys.json).
"""
import argparse
import base64
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def version():
    match = re.search(r'project\(defjam_recomp\s+VERSION\s+(\d+\.\d+\.\d+)', (ROOT / 'CMakeLists.txt').read_text())
    return match.group(1)


def package_files(build, docs_like):
    files = {'defjam_recomp.exe': build / 'defjam_recomp.exe', 'defjam_recomp.pdb': build / 'defjam_recomp.pdb'}
    for rel in ('setup/update.py', 'setup/mods.py', 'setup/mod_catalogue.py', 'config/mods/catalogue-keys.json'):
        files[rel] = ROOT / rel
    if docs_like:
        for doc in sorted(Path(docs_like).glob('*.md')):
            if (ROOT / 'docs' / doc.name).is_file():
                files['docs/' + doc.name] = ROOT / 'docs' / doc.name
    missing = [str(p) for p in files.values() if not p.is_file()]
    if missing:
        sys.exit('Missing: ' + ', '.join(missing))
    return files


def build_zip(files, out):
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for name, path in sorted(files.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            bundle.writestr(info, path.read_bytes())


def sign(manifest, key_path):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    key = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(Path(key_path).read_text().strip()))
    trusted = json.loads((ROOT / 'config/mods/catalogue-keys.json').read_text())['keys']
    if key.public_key().public_bytes_raw().hex() not in trusted:
        sys.exit('This key is not in config/mods/catalogue-keys.json; copies would refuse the update')
    payload = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
    return json.dumps({'payload': base64.b64encode(payload).decode(),
                       'signature': base64.b64encode(key.sign(payload)).decode()}, indent=1) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path, help='folder for the zip and update.json')
    parser.add_argument('--key', help='signing key file (needed with --url)')
    parser.add_argument('--url', help="the uploaded zip's share link")
    parser.add_argument('--build', type=Path, default=ROOT / 'build/win-x64-release')
    parser.add_argument('--docs-like', help="a copy's docs folder: the guides to include")
    parser.add_argument('--notes', default='', help='one line shown in the launcher')
    args = parser.parse_args()
    ver = version()
    args.out.mkdir(parents=True, exist_ok=True)
    zip_path = args.out / f'defjam-{ver}.zip'
    if not zip_path.exists():
        build_zip(package_files(args.build, args.docs_like), zip_path)
        print(f'Wrote {zip_path} ({zip_path.stat().st_size:,} bytes)')
    if not args.url:
        print('Upload it, then run again with --url <its share link> and --key to write update.json')
        return
    if not args.key:
        sys.exit('--key is needed to sign update.json')
    spec = importlib.util.spec_from_file_location('update', ROOT / 'setup/update.py')
    update = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(update)
    update.direct_url(args.url)     # an https link
    manifest = dict(schema=1, game=update.GAME, version=ver,
                    issued=dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), notes=args.notes,
                    package=dict(url=args.url, size=zip_path.stat().st_size,
                                 sha256=hashlib.sha256(zip_path.read_bytes()).hexdigest()))
    document = sign(manifest, args.key)
    update.verify(document, update.trusted_keys())
    (args.out / 'update.json').write_text(document, encoding='utf-8')
    print(f'Wrote {args.out / "update.json"} for {ver}: upload it over the previous update.json')


if __name__ == '__main__':
    main()
