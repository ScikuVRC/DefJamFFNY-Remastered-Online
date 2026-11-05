#!/usr/bin/env python3
"""The game's own updater for a portable copy (Play.bat): check, download, verify, install.

The launcher runs it with the copy's bundled Python, as it runs mods.py. The update channel is
private: whoever builds the game uploads a package zip and a signed update.json wherever they
like (scripts/publish-update.py), and puts update.json's link in each copy's settings.ini
([update] url). Nothing is sent but the requests for those two files.

    python setup/update.py check   --install-dir DIR --data-dir DIR --runtime 0.15.0 --url URL
    python setup/update.py install --install-dir DIR --data-dir DIR --runtime 0.15.0 --url URL
    python setup/update.py cleanup --install-dir DIR

update.json: {"payload": base64 of the manifest's exact bytes, "signature": base64 Ed25519 over
them}, signed with a key in config/mods/catalogue-keys.json. The manifest:
{"schema": 1, "game": "defjam-remastered-update", "version": "0.16.0", "issued": "...",
 "notes": "...", "package": {"url": "...", "size": N, "sha256": "..."}}.

The package is a zip of the game's own files only (the program, its symbols, docs and helper
scripts), never game/ or data/. Install replaces them in the copy: a file Windows keeps open
(the running program) is renamed to <name>.old first, which Windows allows, and removed at the
next start (cleanup). If any step fails, everything moved is put back.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

KEYS = Path(__file__).resolve().parents[1] / 'config/mods/catalogue-keys.json'
GAME = 'defjam-remastered-update'
MAX_MANIFEST = 1024 * 1024
MAX_PACKAGE = 1024 ** 3
MAX_FILES = 2000
# What a package may replace, relative to the copy. Never game/ (the dump) or data/ (the
# player's saves, settings and mods).
ALLOWED = re.compile(r'(defjam_recomp\.(exe|pdb)|[A-Za-z0-9_]+\.dll|README\.txt|Play\.bat'
                     r'|Import PS2 saves\.bat|(docs|setup|config|tools)/[A-Za-z0-9_./ -]+)')


class UpdateError(ValueError):
    pass


def check(condition, message):
    if not condition:
        raise UpdateError(message)


def semver(value):
    match = re.fullmatch(r'v?(\d+)\.(\d+)\.(\d+)', str(value).strip())
    check(match, f'Not a version: {value}')
    return tuple(map(int, match.groups()))


def direct_url(url):
    """A share link as a direct download: Dropbox ?dl=1, Google Drive's uc?export=download."""
    url = url.strip()
    parts = urllib.parse.urlsplit(url)
    local_test = (os.environ.get('RECOMP_UPDATE_TEST') == '1' and parts.scheme == 'http'
                  and parts.hostname in ('127.0.0.1', 'localhost'))      # a test's own server only
    check((parts.scheme == 'https' or local_test) and parts.hostname and not parts.username and not parts.password,
          'The update address must be an https link')
    host = parts.hostname.lower()
    if host.endswith('dropbox.com'):
        query = [(k, v) for k, v in urllib.parse.parse_qsl(parts.query) if k != 'dl'] + [('dl', '1')]
        return urllib.parse.urlunsplit((parts.scheme, parts.netloc, parts.path, urllib.parse.urlencode(query), ''))
    if host == 'drive.google.com':
        match = re.match(r'/file/d/([A-Za-z0-9_-]+)', parts.path)
        file_id = match.group(1) if match else dict(urllib.parse.parse_qsl(parts.query)).get('id')
        if file_id:
            return 'https://drive.google.com/uc?export=download&id=' + file_id
    return url


def open_url(url, headers=None):
    request = urllib.request.Request(direct_url(url), headers={'User-Agent': 'DefJamUpdate/1', **(headers or {})})
    return urllib.request.urlopen(request, timeout=30)


def fetch(url, limit):
    with open_url(url) as response:
        data = response.read(limit + 1)
    check(len(data) <= limit, 'The update file is too large')
    return data


def trusted_keys():
    try:
        return json.loads(KEYS.read_text(encoding='utf-8')).get('keys', [])
    except (OSError, ValueError):
        return []


def verify(document, keys):
    """The manifest of an update.json, once its signature checks out."""
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    try:
        outer = json.loads(document)
        payload = base64.b64decode(outer['payload'], validate=True)
        signature = base64.b64decode(outer['signature'], validate=True)
    except (ValueError, KeyError, TypeError):
        raise UpdateError('The update file is damaged')
    check(keys, 'No update signing key in this copy')
    for key in keys:
        try:
            Ed25519PublicKey.from_public_bytes(bytes.fromhex(key)).verify(signature, payload)
            break
        except (InvalidSignature, ValueError):
            continue
    else:
        raise UpdateError('The update is not signed by this game\'s publisher; nothing was changed')
    manifest = json.loads(payload)
    check(manifest.get('schema') == 1 and manifest.get('game') == GAME, 'Not an update for this game')
    semver(manifest.get('version'))
    package = manifest.get('package') or {}
    check(isinstance(package.get('url'), str) and type(package.get('size')) is int and 0 < package['size'] <= MAX_PACKAGE
          and re.fullmatch(r'[0-9a-f]{64}', str(package.get('sha256'))), 'The update file is incomplete')
    return manifest


class Progress:
    def __init__(self, data_dir):
        self.root = Path(data_dir) / 'updater'
        self.root.mkdir(parents=True, exist_ok=True)

    def write(self, name, sections):
        lines = []
        for section, values in sections.items():
            lines.append(f'[{section}]')
            lines += [f'{k}={str(v).replace(chr(10), " ").replace(chr(13), " ")[:4000]}' for k, v in values.items()]
        tmp = self.root / (name + '.tmp')
        tmp.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        # The launcher reads these files while the job runs, and Windows refuses to replace
        # a file another program has open: try again for a moment. Running the job
        # under the launcher, the first download died at 2 MB on that refusal.
        for attempt in range(50):
            try:
                os.replace(tmp, self.root / name)
                return True
            except PermissionError:
                time.sleep(0.01)
        return False

    def progress(self, message, done=0, total=0):
        # Progress only informs the launcher: a write that still fails costs one update of it.
        try:
            self.write('progress.ini', {'progress': {'message': message, 'done': done, 'total': total}})
        except OSError:
            pass

    def view(self, **values):
        self.write('view.ini', {'update': values})

    def cancelled(self):
        return (self.root / 'cancel').exists()


def do_check(args, progress):
    progress.progress('Checking for an update')
    manifest = verify(fetch(args.url, MAX_MANIFEST), trusted_keys())
    newer = semver(manifest['version']) > semver(args.runtime)
    progress.view(state='available' if newer else 'current', version=manifest['version'],
                  installed=args.runtime, notes=manifest.get('notes', ''), size=manifest['package']['size'],
                  message=(f'Version {manifest["version"]} is available' if newer else
                           f'This is the latest version ({args.runtime})'))
    progress.progress('Update available' if newer else 'Up to date')
    return manifest


def download(manifest, folder, progress):
    package = manifest['package']
    target = Path(folder) / 'package.zip'
    digest = hashlib.sha256()
    count = 0
    with open_url(package['url']) as response, target.open('wb') as out:
        while chunk := response.read(1 << 20):
            check(not progress.cancelled(), 'Cancelled; nothing was changed')
            count += len(chunk)
            check(count <= package['size'], 'The download is larger than the update says')
            digest.update(chunk)
            out.write(chunk)
            progress.progress('Downloading the update', count, package['size'])
    check(count == package['size'] and digest.hexdigest() == package['sha256'],
          'The download does not match the update (size or SHA-256); nothing was changed')
    return target


def entries(archive):
    with zipfile.ZipFile(archive) as bundle:
        rows = [r for r in bundle.infolist() if not r.is_dir()]
        check(0 < len(rows) <= MAX_FILES, 'The update package is empty or too large')
        names = []
        for row in rows:
            name = row.filename
            parts = PurePosixPath(name).parts
            check('\\' not in name and not name.startswith('/') and '..' not in parts and ALLOWED.fullmatch(name)
                  and not row.flag_bits & 1, f'The update package holds a file it may not: {name}')
            names.append(name)
        check(len({n.casefold() for n in names}) == len(names), 'The update package lists a file twice')
        check('defjam_recomp.exe' in names, 'The update package has no game program')
    return names


def install_files(archive, names, install_dir, staging):
    """Move the package's files into the copy; on any failure put everything back."""
    install_dir, staging = Path(install_dir), Path(staging)
    with zipfile.ZipFile(archive) as bundle:
        for name in names:
            out = staging / 'new' / name
            out.parent.mkdir(parents=True, exist_ok=True)
            with bundle.open(name) as source, out.open('wb') as target:
                shutil.copyfileobj(source, target, 1 << 20)
    done = []           # (target, kept): kept is where the old file went, or None
    try:
        for name in names:
            target = install_dir / name
            kept = None
            if target.exists():
                kept = target.with_name(target.name + '.old')
                if kept.exists():
                    kept.unlink()
                os.replace(target, kept)    # a running program can be renamed, not overwritten
            target.parent.mkdir(parents=True, exist_ok=True)
            done.append((target, kept))
            os.replace(staging / 'new' / name, target)
    except OSError as error:
        for target, kept in reversed(done):
            try:
                if target.exists():
                    target.unlink()
                if kept:
                    os.replace(kept, target)
            except OSError:
                pass
        raise UpdateError(f'Could not put the update in place ({error}); the previous version was kept')
    return done


def do_install(args, progress):
    manifest = do_check(args, progress)
    check(semver(manifest['version']) > semver(args.runtime), 'Already up to date')
    install_dir = Path(args.install_dir)
    check((install_dir / 'defjam_recomp.exe').is_file() and (install_dir / 'game').is_dir(),
          'This is not a portable copy of the game (no defjam_recomp.exe beside game)')
    with tempfile.TemporaryDirectory(prefix='update-', dir=install_dir) as staging:
        archive = download(manifest, staging, progress)
        names = entries(archive)
        progress.progress('Installing the update')
        install_files(archive, names, install_dir, staging)
    progress.view(state='installed', version=manifest['version'], installed=args.runtime,
                  notes=manifest.get('notes', ''), size=manifest['package']['size'],
                  message=f'Updated to {manifest["version"]}: restart to use it')
    progress.progress(f'Updated to {manifest["version"]}: restart to use it')


def do_cleanup(install_dir):
    """The files an update renamed aside, now that the program that held them has ended."""
    removed = 0
    for path in Path(install_dir).rglob('*.old'):
        try:
            rel = path.relative_to(install_dir).as_posix()
        except ValueError:
            continue
        if ALLOWED.fullmatch(rel[:-4]):
            try:
                path.unlink()
                removed += 1
            except OSError:
                pass
    return removed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('check', 'install', 'cleanup'))
    parser.add_argument('--install-dir', required=True)
    parser.add_argument('--data-dir')
    parser.add_argument('--runtime', default='0.0.0')
    parser.add_argument('--url', default='')
    args = parser.parse_args(argv)
    if args.command == 'cleanup':
        print(json.dumps(dict(event='complete', removed=do_cleanup(args.install_dir))))
        return 0
    progress = Progress(args.data_dir or Path(args.install_dir) / 'data')
    try:
        (progress.root / 'cancel').unlink(missing_ok=True)
        do_cleanup(args.install_dir)        # what the last update renamed aside
        check(args.url, 'No update address: set [update] url in settings.ini')
        (do_check if args.command == 'check' else do_install)(args, progress)
        return 0
    except (UpdateError, OSError, urllib.error.URLError, zipfile.BadZipFile, ImportError) as error:
        message = str(error.reason) if isinstance(error, urllib.error.URLError) and hasattr(error, 'reason') else str(error)
        progress.progress(message)
        progress.view(state='error', installed=args.runtime, message=message)
        print(json.dumps(dict(event='error', message=message)))
        return 1


if __name__ == '__main__':
    sys.exit(main())
