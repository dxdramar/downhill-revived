"""SIMDe for the Mac build (finding 075): the port's SSE intrinsics built from wasm SIMD.

    python port/tools/simde.py          prints the include folder

link_mac.py calls include() (the port's SSE code includes <x86/sse2.h>), build_mac.py root()
(wasm2c's output includes <simde/wasm/simd128.h>). It downloads SIMDe's amalgamated release once
(pinned version, SHA-256 checked; nothing else is accepted) into build/downloads and unpacks it
under build/port-mac/simde/simde. SIMDe is MIT-licensed (port/release/THIRD-PARTY-NOTICES.txt)."""
import hashlib
import tarfile
import urllib.request
from pathlib import Path

VERSION = '0.8.2'
URL = 'https://github.com/simd-everywhere/simde/releases/download/v%s/simde-amalgamated-%s.tar.xz' % (VERSION, VERSION)
SHA256 = '59068edc3420e75c5ff85ecfd80a77196fb3a151227a666cc20abb313a5360bf'

ROOT = Path(__file__).resolve().parents[2]
DOWNLOADS = ROOT / 'build' / 'downloads'
OUT = ROOT / 'build' / 'port-mac' / 'simde' / 'simde'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def include():
    """The folder that holds x86/sse2.h, fetched and unpacked if need be."""
    if (OUT / 'x86' / 'sse2.h').exists():
        return OUT
    archive = DOWNLOADS / URL.rsplit('/', 1)[1]
    if not archive.exists():
        DOWNLOADS.mkdir(parents=True, exist_ok=True)
        part = archive.with_suffix('.part')
        urllib.request.urlretrieve(URL, part)
        part.rename(archive)
    if sha256(archive) != SHA256:
        raise SystemExit('%s: SHA-256 is not the pinned %s' % (archive, SHA256))
    OUT.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as t:
        for m in t.getmembers():
            parts = Path(m.name).parts[1:]  # simde-amalgamated-<version>/...
            if not parts or m.issym() or m.islnk() or '..' in parts:
                continue
            m.name = str(Path(*parts))
            t.extract(m, OUT, filter='data')
    return OUT


def root():
    """The folder that holds simde/."""
    return include().parent


if __name__ == '__main__':
    print(include())
