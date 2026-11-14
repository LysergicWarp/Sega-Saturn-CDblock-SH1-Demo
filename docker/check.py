#!/usr/bin/env python3
"""Run a disc headless on the Linux core for a number of frames, in Docker, and time the run.

The core and the runner are build.py's, run in its image. The CD-block ROM goes in the core's
system directory as cdblock.bin, so the emulator library runs the CD block, unless --hle leaves it
out for Beetle Saturn's own high-level CD block. --card puts an MPEG card ROM in the card slot.

Output in build/checks/<disc name> in the demo's folder, or the folder --output names, which is
emptied first: result.json from the runner, log.txt with the core's log and the elapsed time,
transcript.txt with --transcript, and each frame --screenshots names as frame-<number>.png.

usage:
  check.py --disc IMAGE --bios BIOS --cd-block ROM [options]

examples:
  check.py --disc dist/scroller.cue --card dist/card-rom.bin --bios bios.bin --cd-block cdb105.bin
  check.py --disc game.cue --bios bios.bin --cd-block cdb105.bin --screenshots 600 1800
  check.py --disc game.cue --bios bios.bin --hle
"""

import argparse
import shlex
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / 'build'

IMAGE = 'cd-block-sh1-demo/beetle-build:trixie-1'
CORE = 'mednafen_saturn_libretro.so'
RUNNER = 'beetle-runner'


class Failure(Exception):
    pass


def png(bitmap):
    """The runner's screenshot, a 32-bit BMP file's bytes, as a PNG file's."""
    start, = struct.unpack_from('<I', bitmap, 10)
    width, height = struct.unpack_from('<ii', bitmap, 18)
    # A negative height stores the top row first, a positive one the bottom row
    order = range(-height) if height < 0 else range(height - 1, -1, -1)
    lines = bytearray()
    for row in order:
        pixels = bitmap[start + row * width * 4:start + (row + 1) * width * 4]
        line = bytearray(width * 3)
        line[0::3], line[1::3], line[2::3] = pixels[2::4], pixels[1::4], pixels[0::4]
        lines += b'\0' + line     # each line unfiltered

    def chunk(kind, body):
        return (struct.pack('>I', len(body)) + kind + body
                + struct.pack('>I', zlib.crc32(kind + body)))

    header = struct.pack('>IIBBBBB', width, abs(height), 8, 2, 0, 0, 0)     # 8-bit RGB
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
            + chunk(b'IDAT', zlib.compress(bytes(lines), 9)) + chunk(b'IEND', b''))


def mount(source, target, readonly=True):
    return ['--mount', f'type=bind,source={source},target={target}'
            + (',readonly' if readonly else '')]


def check(options):
    build = options.build.resolve()
    for name in (CORE, RUNNER):
        if not (build / name).is_file():
            raise Failure(f'no {name} in {build}: run build.py first')
    for path in (options.disc, options.bios, options.cd_block, options.card):
        if path is not None and not path.is_file():
            raise Failure(f'no file {path}')
    disc = options.disc.resolve()
    output = (options.output or BUILD / 'checks' / disc.stem).resolve()
    if output.exists():
        shutil.rmtree(output)
    system = output / 'system'
    system.mkdir(parents=True)

    # The runner copies each system file into the core's system directory under its own name
    shutil.copyfile(options.bios, system / 'bios.bin')
    runner = [f'/build/{RUNNER}', '--core', f'/build/{CORE}', '--image', f'/disc/{disc.name}',
              '--bios', '/system/bios.bin', '--region', options.region,
              '--frames', str(options.frames)]
    if not options.hle:
        shutil.copyfile(options.cd_block, system / 'cdblock.bin')
        runner += ['--system-file', '/system/cdblock.bin']
    if options.card:
        shutil.copyfile(options.card, system / 'mpeg_card.bin')
        runner += ['--system-file', '/system/mpeg_card.bin',
                   '--core-option', 'beetle_saturn_mpeg_card=enabled']
    for frame in options.screenshots:
        runner += ['--screenshot', f'{frame}:/out/frame-{frame}.bmp']

    environment = []
    if options.lenient:
        environment += ['-e', 'CDB_LLE_LENIENT=1']
    if options.transcript:
        environment += ['-e', 'CDB_LLE_TRANSCRIPT=/out/transcript.txt']
    if options.transcript_data:
        environment += ['-e', 'CDB_LLE_TRANSCRIPT_DATA=1']

    script = f"TIMEFORMAT='elapsed %R s'; time {shlex.join(runner)} > /out/result.json"
    command = ['docker', 'run', '--rm', *environment, *mount(build, '/build'),
               *mount(disc.parent, '/disc'), *mount(system, '/system'),
               *mount(output, '/out', readonly=False), IMAGE, 'bash', '-c', script]
    log = output / 'log.txt'
    with open(log, 'wb') as file:
        code = subprocess.run(command, stderr=file).returncode
    shutil.rmtree(system)

    for bitmap in output.glob('*.bmp'):
        bitmap.with_suffix('.png').write_bytes(png(bitmap.read_bytes()))
        bitmap.unlink()

    lines = log.read_text(encoding='utf-8', errors='replace').splitlines()
    elapsed = next((line for line in lines if line.startswith('elapsed')), 'no elapsed time')
    print(f'Ran {disc.name} for {options.frames} frames: exit {code}, {elapsed}. '
          f'Output in {output}')
    return code


def main():
    parser = argparse.ArgumentParser(
        usage=argparse.SUPPRESS, description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--disc', required=True, type=Path, metavar='IMAGE',
                        help='the disc image, in any format Beetle Saturn loads')
    parser.add_argument('--bios', required=True, type=Path, metavar='BIOS',
                        help='the IPL BIOS image')
    parser.add_argument('--cd-block', type=Path, metavar='ROM',
                        help='the CD-block ROM image, needed unless --hle')
    parser.add_argument('--card', type=Path, metavar='ROM',
                        help="an MPEG card ROM, put in the emulator's card slot")
    parser.add_argument('--region', choices=('eu', 'jp', 'na'), default='na',
                        help="the console's region: na by default")
    parser.add_argument('--frames', type=int, default=1800, metavar='COUNT',
                        help='the frames to run, 1800 by default')
    parser.add_argument('--screenshots', type=int, nargs='+', default=[], metavar='FRAME',
                        help='the frames to save as PNG files, the first frame 1')
    parser.add_argument('--lenient', action='store_true',
                        help="report the emulator's strict checks in the core's log instead of "
                             'stopping the CD block')
    parser.add_argument('--transcript', action='store_true',
                        help="write the host's CD block commands and responses to transcript.txt")
    parser.add_argument('--transcript-data', action='store_true',
                        help='also write every data port word the host reads to the transcript')
    parser.add_argument('--hle', action='store_true',
                        help="run Beetle Saturn's own high-level CD block instead, to compare")
    parser.add_argument('--build', type=Path, default=BUILD / 'beetle', metavar='DIR',
                        help="the folder with build.py's core and runner, by default "
                             "build/beetle in the demo's folder")
    parser.add_argument('--output', type=Path, metavar='DIR',
                        help='the output folder, emptied first, by default '
                             "build/checks/<disc name> in the demo's folder")
    options = parser.parse_args()
    if options.transcript_data and not options.transcript:
        parser.error('--transcript-data needs --transcript')
    if options.hle and (options.card or options.lenient or options.transcript):
        parser.error('--card, --lenient and --transcript apply to the emulator, not to --hle')
    if not options.hle and not options.cd_block:
        parser.error('--cd-block names the CD-block ROM, needed unless --hle')
    if options.frames < 1:
        parser.error('--frames is at least 1')
    for frame in options.screenshots:
        if not 1 <= frame <= options.frames:
            parser.error(f'--screenshots {frame} is outside the run, frames 1 to {options.frames}')
    try:
        code = check(options)
    except (Failure, OSError) as error:
        sys.exit(f'check.py: {error}')
    sys.exit(code)


if __name__ == '__main__':
    main()
