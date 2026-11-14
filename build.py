#!/usr/bin/env python3
"""Build the demo: the card firmware's ROM, the modules' packages and the demo's disc images.

The SH-2 and SH-1 code builds in the yaul Docker image, pinned below, with this directory mounted.
The image has no Python, so the steps that encode the card ROM and the packages, that make the
benchmark's data around the image's PRS compressor and that encrypt the card-library disc's file
for the cipher module, run here. Docker must be running.

Output in dist/ beside this script: card-rom.bin, the card ROM with the modules' library, and the
discs, each a .cue with its image: cd-block-sh1-demo, the benchmark, card-library, the card
ROM's modules, and scroller, a streamed picture. The packages stay in src/modules-asm/build/. The
discs' volume dates are fixed, their program files' dates are the build's. --firmware c puts the
card firmware's C version in the card ROM instead of the assembly one, built with the SH-1 GCC of
the Dockerfile here.

usage:
  build.py [--firmware asm|c] [--clean]

examples:
  build.py
  build.py --firmware c
  build.py --clean
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DIST = HERE / 'dist'
TOOLS = HERE / 'tools'

# libyaul 0.3.1 with its SH-2 toolchain, the image the demo's code is built and checked with.
IMAGE = 'ijacquez/yaul@sha256:b6d272204767cd0a734139dd3a2ab298ef5912fb7b7224b78e6d9fac1a7825a6'

# The disc's volume dates, 2026-10-26 00:00 UTC, in place of the build's time.
SOURCE_DATE_EPOCH = '1792972800'

# The card firmware in assembly, or in C, built with the SH-1 GCC of the Dockerfile beside this
# script, an image built here when first asked for.
FIRMWARES = {
    'asm': HERE / 'src' / 'card-firmwares' / 'card-firmware-asm',
    'c': HERE / 'src' / 'card-firmwares' / 'card-firmware-c',
}
SH1_IMAGE = 'cd-block-sh1-demo/sh1-gcc:13.2.0-1'
MODULES = HERE / 'src' / 'modules-asm'
MODULE_NAMES = ('decompress', 'transform', 'cipher')     # MODULES in its Makefile
DEMO = HERE / 'src' / 'demo-discs' / 'demo'
BENCH = DEMO / 'build' / 'bench'
DISC = 'cd-block-sh1-demo'     # SH_PROGRAM in its Makefile
# The second disc, the card ROM's module library
LIBRARY_DEMO = HERE / 'src' / 'demo-discs' / 'card-library'
LIBRARY_DISC = 'card-library'     # SH_PROGRAM in its Makefile
# Its main.c encrypted for the cipher module's job, with the stream number its job asks for
LIBRARY_ENCRYPTED = LIBRARY_DEMO / 'build' / 'main.xtea'
CIPHER_STREAM = '291'     # CIPHER_STREAM in its main.c
# The third disc, a picture streamed in encrypted strips
SCROLLER = HERE / 'src' / 'demo-discs' / 'scroller'
SCROLLER_DISC = 'scroller'     # SH_PROGRAM in its Makefile


class Failure(Exception):
    pass


def run(command):
    print('>', ' '.join(str(part) for part in command), flush=True)
    if subprocess.run(command).returncode != 0:
        raise Failure(f'{command[0]} failed')


def container(directory, *command, image=IMAGE):
    """command in image, in directory, a directory of the demo's.

    The image runs as root, so build/ is made here first: the steps here write into it, which a
    directory made by the container's root would refuse on a POSIX host.
    """
    (directory / 'build').mkdir(exist_ok=True)
    work = '/work/' + directory.relative_to(HERE).as_posix()
    run(['docker', 'run', '--rm', '-e', f'SOURCE_DATE_EPOCH={SOURCE_DATE_EPOCH}',
         '-v', f'{HERE}:/work', '-w', work, image, *command])


def make(directory, *targets, image=IMAGE):
    container(directory, 'make', *targets, image=image)


def python(script, *arguments):
    run([sys.executable, TOOLS / script, *arguments])


def build(firmware):
    make(MODULES, *(f'build/{name}-module.bin' for name in MODULE_NAMES))
    packages = [MODULES / 'build' / f'{name}-package.bin' for name in MODULE_NAMES]
    for name, package in zip(MODULE_NAMES, packages):
        python('package.py', MODULES / 'build' / f'{name}-module.bin', package)
    directory = FIRMWARES[firmware]
    if firmware == 'c':
        # Cached by Docker after the first build, which compiles GCC
        run(['docker', 'build', '--tag', SH1_IMAGE, HERE])
        make(directory, 'build/card-firmware.bin', image=SH1_IMAGE)
    else:
        make(directory, 'build/card-firmware.bin')
    card_rom = directory / 'build' / 'card-rom.bin'
    python('payload.py', 'card', directory / 'build' / 'card-firmware.bin', card_rom)
    # The modules in the card ROM too, the library's handles from 251
    python('library.py', card_rom, *packages)
    # The benchmark's disc file and its chunk table, which the demo's Makefile takes
    python('bench.py', 'chunks', BENCH)
    container(DEMO, 'sh', '-c', 'for f in build/bench/chunk-*.bin; do '
              '"$YAUL_INSTALL_ROOT/bin/bcl_prs" "$f" "${f%.bin}.prs" > /dev/null || exit 1; done')
    python('bench.py', 'pack', BENCH, DEMO / 'build' / 'bench.prs',
           DEMO / 'build' / 'bench-table.bin')
    make(DEMO)
    LIBRARY_ENCRYPTED.parent.mkdir(exist_ok=True)
    python('cipher.py', LIBRARY_DEMO / 'main.c', LIBRARY_ENCRYPTED, '--stream', CIPHER_STREAM)
    make(LIBRARY_DEMO)
    make(SCROLLER)
    DIST.mkdir(exist_ok=True)
    for path in (card_rom, DEMO / f'{DISC}.iso', DEMO / f'{DISC}.cue',
                 LIBRARY_DEMO / f'{LIBRARY_DISC}.iso', LIBRARY_DEMO / f'{LIBRARY_DISC}.cue',
                 SCROLLER / f'{SCROLLER_DISC}.iso', SCROLLER / f'{SCROLLER_DISC}.cue'):
        shutil.copy2(path, DIST / path.name)
        print(f'{DIST / path.name}: {path.stat().st_size} bytes')


def clean():
    # Each Makefile's clean needs only make, which the yaul image has. What it leaves goes in the
    # container too, whose root made it: build/ with the steps' data, the disc's files in cd/ and
    # its audio tracks
    for directory in (*FIRMWARES.values(), MODULES, DEMO, LIBRARY_DEMO, SCROLLER):
        make(directory, 'clean')
        container(directory, 'rm', '-rf', 'build', 'cd', 'audio-tracks')
    shutil.rmtree(DIST, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(
        usage=argparse.SUPPRESS, description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--clean', action='store_true',
                        help='remove the build output instead, dist/ included')
    parser.add_argument('--firmware', choices=FIRMWARES, default='asm',
                        help='the card firmware: asm, the default, or c')
    options = parser.parse_args()
    try:
        if options.clean:
            clean()
        else:
            build(options.firmware)
    except (Failure, OSError) as error:
        sys.exit(f'build.py: {error}')


if __name__ == '__main__':
    main()
