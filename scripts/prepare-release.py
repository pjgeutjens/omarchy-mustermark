#!/usr/bin/env python3
"""Capture the current source tree for local review; does not publish or commit."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path, help='New output directory outside the repository')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
out = args.output.resolve()
if out == repo or repo in out.parents:
    parser.error('Output must be outside the repository')
version = re.search(r'project\(mustermark VERSION ([0-9.]+)', (repo / 'CMakeLists.txt').read_text())[1]
files = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=repo).decode().split('\0')
files = sorted({f for f in files if f and (repo / f).exists() and not f.startswith('release-evidence/') and not f.endswith('PLAN.md')})
out.mkdir(parents=True, exist_ok=False)
archive = out / f'mustermark-{version}.tar.gz'
manifest = []
with archive.open('wb') as raw, gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as compressed, tarfile.open(fileobj=compressed, mode='w') as tar:
    for name in files:
        path = repo / name
        if path.is_symlink() or not path.is_file():
            raise SystemExit(f'Refusing non-regular source file: {name}')
        data = path.read_bytes()
        info = tarfile.TarInfo(f'mustermark-{version}/{name}')
        info.size = len(data)
        info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
        tar.addfile(info, io.BytesIO(data))
        manifest.append({'path': name, 'sha256': hashlib.sha256(data).hexdigest()})
(out / 'SHA256SUMS').write_text(f'{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n')
(out / 'manifest.json').write_text(json.dumps({'base': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo).decode().strip(), 'version': version, 'files': manifest}, indent=2) + '\n')
print(archive)
