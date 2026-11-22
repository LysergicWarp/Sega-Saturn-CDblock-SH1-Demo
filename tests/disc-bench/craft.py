#!/usr/bin/env python3
"""Build the disc decode benchmark with the card firmware off the disc: a disc whose Saturn header
names the bootstrap in src/disc-extensions/firmware-boot-asm/ as its disc extension, for a console
with no MPEG card.

Run build.py first: the benchmark's file and chunk table come from src/demo-discs/demo/build/.

Steps, in the demo's yaul image where they assemble or link: the assembly card firmware, the
bootstrap that carries it, encoded with tools/payload.py into cd/SH1EXT.BIN, the decompress module
and its package from tools/package.py, this program and its image, then the image's header pointed
at SH1EXT.BIN: +0xf8 its FAD (LBA + 150), +0xfc its encoded length, both big-endian.

Output: disc-bench.cue and .iso beside this script. Docker must be running.
"""

import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEMO = HERE.parents[1]
sys.path.insert(0, str(DEMO))
from build import IMAGE  # noqa: E402

TOOLS = DEMO / 'tools'
FIRMWARE = DEMO / 'src' / 'card-firmwares' / 'card-firmware-asm'
BOOT = DEMO / 'src' / 'disc-extensions' / 'firmware-boot-asm'
MODULES = DEMO / 'src' / 'modules-asm'
BENCH = DEMO / 'src' / 'demo-discs' / 'demo' / 'build'
ISO = HERE / 'disc-bench.iso'
NAME = 'SH1EXT.BIN'
SECTOR = 2048
LEAD = 150


def run(command):
    print('>', ' '.join(str(part) for part in command), flush=True)
    if subprocess.run(command).returncode != 0:
        sys.exit(f'craft.py: {command[0]} failed')


def make(directory, *targets):
    """make in the yaul image. build/ is made here first, as build.py does."""
    (directory / 'build').mkdir(exist_ok=True)
    work = '/work/' + directory.relative_to(DEMO).as_posix()
    run(['docker', 'run', '--rm', '-v', f'{DEMO}:/work', '-w', work, IMAGE, 'make', *targets])


def extent(iso, name):
    """LBA and size of a file in the image's root directory."""
    with open(iso, 'rb') as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        if pvd[1:6] != b'CD001':
            sys.exit('craft.py: no ISO 9660 volume descriptor')
        lba, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
        f.seek(lba * SECTOR)
        data = f.read(size)
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        record = data[pos:pos + length]
        ident = record[33:33 + record[32]].decode('ascii', 'replace').split(';')[0]
        if ident == name:
            return struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
        pos += length
    sys.exit(f'craft.py: {name} not in the image')


def main():
    for path in (BENCH / 'bench.prs', BENCH / 'bench-table.bin'):
        if not path.exists():
            sys.exit(f'craft.py: {path} missing, run build.py first')
    make(FIRMWARE, 'build/card-firmware.bin')
    make(BOOT)
    make(MODULES, 'build/decompress-module.bin')
    run([sys.executable, TOOLS / 'package.py', MODULES / 'build' / 'decompress-module.bin',
         MODULES / 'build' / 'decompress-package.bin'])
    (HERE / 'cd').mkdir(exist_ok=True)
    payload = HERE / 'cd' / NAME
    run([sys.executable, TOOLS / 'payload.py', 'encode', BOOT / 'build' / 'firmware-boot.bin',
         payload])
    ISO.unlink(missing_ok=True)
    make(HERE)
    lba, size = extent(ISO, NAME)
    if size != payload.stat().st_size:
        sys.exit(f'craft.py: {NAME} is {size} bytes in the image')
    fad = lba + LEAD
    image = bytearray(ISO.read_bytes())
    if not image.startswith(b'SEGA SEGASATURN '):
        sys.exit('craft.py: no Saturn header at the image start')
    if image[0xF8:0x100] != bytes(8):
        sys.exit('craft.py: header +0xf8..+0xff already set')
    struct.pack_into('>II', image, 0xF8, fad, size)
    # The container wrote the image, as root on a POSIX host with Docker Engine, so it cannot be
    # written in place: the patched image goes into a new file, renamed over it.
    patched = ISO.with_suffix('.tmp')
    patched.write_bytes(image)
    patched.replace(ISO)
    print(f'{ISO.name}: {NAME} at LBA {lba}, FAD {fad} ({fad:#x}), {size} bytes encoded')


if __name__ == '__main__':
    main()
