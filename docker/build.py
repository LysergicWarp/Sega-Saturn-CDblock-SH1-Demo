#!/usr/bin/env python3
"""Build the Beetle Saturn fork's core with the CD-block emulator library, in Docker.

The fork's checkout is copied into a Docker volume without its Git metadata and built there with
HAVE_CDB_LLE=1 and LTO, in the image of the Dockerfile here, so the host needs no toolchain and
rebuilds are incremental. The library is the checkout's deps/cd-block-sh1-emulator submodule, or
the folder --library names, whose include/ and src/ then take the submodule's place.

One volume holds every build: a folder for the Linux builds and one for the Windows builds, and in
each a folder for every checkout and library given, so builds of different sources do not share
object files.

Without --windows it builds the Linux core, mednafen_saturn_libretro.so, and beetle-runner, the
headless runner check.py runs it with, from beetle-runner.c here. Output in build/beetle in the
demo's folder.

With --windows it cross-builds mednafen_saturn_libretro.dll with MinGW-w64 GCC. Beetle Saturn's
Makefile leaves LTO off for Windows, and LTO=1 turns it on. Output in build/beetle-windows. The
core needs libwinpthread-1.dll, which RetroArch's Windows builds ship beside retroarch.exe.

Both write revision.txt, the checkout's commit. Docker must be running.

usage:
  build.py --source CHECKOUT [--windows] [--library DIR] [--output DIR] [--clean]

examples:
  build.py --source ../beetle-saturn
  build.py --source ../beetle-saturn --windows
  build.py --source ../beetle-saturn --library ../emulator --clean
"""

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / 'build'

IMAGE = 'cd-block-sh1-demo/beetle-build:trixie-1'
# The work volume, mounted at /work
VOLUME = 'cd-block-sh1-demo-beetle-work'
# The fork's Makefile reads the library from here
LIBRARY = 'deps/cd-block-sh1-emulator'

# Each target's folder in the work volume, output folder, make arguments and core
LINUX = ('linux', BUILD / 'beetle', 'HAVE_CDB_LLE=1', 'mednafen_saturn_libretro.so')
WINDOWS = ('windows', BUILD / 'beetle-windows',
           'platform=win LTO=1 HAVE_CDB_LLE=1 '
           'CC=x86_64-w64-mingw32-gcc-posix CXX=x86_64-w64-mingw32-g++-posix',
           'mednafen_saturn_libretro.dll')

RUNNER = ('gcc -std=c11 -O2 -Wall -Wextra -Werror -I{work}/libretro-common/include '
          '\'-DBEETLE_REVISION="{revision}"\' /runner/beetle-runner.c -ldl -o /out/beetle-runner')


class Failure(Exception):
    pass


def git(source, *arguments):
    """git's output in the checkout, which another user may own."""
    command = ['git', '-c', f'safe.directory={source.as_posix()}', '-C', str(source), *arguments]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise Failure(f'no Git checkout at {source}')
    return result.stdout.strip()


def mount(source, target, kind='bind', readonly=True):
    return ['--mount', f'type={kind},source={source},target={target}'
            + (',readonly' if readonly else '')]


def tree(source, library):
    """A build's folder name in the work volume: the checkout's name and a hash of the paths."""
    paths = str(source) + (f'|{library}' if library else '')
    name = re.sub(r'[^A-Za-z0-9._-]', '-', source.name)
    return f'{name}-{hashlib.sha256(paths.encode()).hexdigest()[:8]}'


def build(source, library, windows, output, clean):
    system, default_output, make_arguments, core = WINDOWS if windows else LINUX
    output = (output or default_output).resolve()
    if not (source / 'Makefile').is_file():
        raise Failure(f'no Beetle Saturn checkout at {source}')
    work = f'/work/{system}/{tree(source, library)}'
    own = library is None
    if own:
        library = source / LIBRARY
    if not (library / 'include').is_dir() or not (library / 'src').is_dir():
        raise Failure(f'no emulator library at {library}: clone the fork with '
                      '--recurse-submodules, or name the library with --library' if own else
                      f'no emulator library at {library}')
    revision = git(source, 'rev-parse', 'HEAD')
    changed = ' with local changes' if git(source, 'status', '--porcelain') else ''
    print(f'Building Beetle {revision}{" for Windows" if windows else ""}{changed} in {work}',
          flush=True)

    if subprocess.run(['docker', 'build', '--tag', IMAGE, str(HERE)]).returncode != 0:
        raise Failure('the build image failed')
    output.mkdir(parents=True, exist_ok=True)

    copy = f'tar -C /src --exclude=.git --exclude=.vs {{skip}}-cf - . | tar -C {work} -xf -'
    script = ['set -euo pipefail']
    if clean:
        script += [f'rm -rf {work}']
    script += [f'mkdir -p {work}']
    mounts = mount(source, '/src')
    if own:
        script += [copy.format(skip='')]
    else:
        script += [copy.format(skip=f'--exclude=./{LIBRARY} '),
                   f'mkdir -p {work}/{LIBRARY}',
                   f'tar -C /cdblock -cf - include src | tar -C {work}/{LIBRARY} -xf -']
        mounts += mount(library, '/cdblock')
    script += [f'make -C {work} -j$(nproc) {make_arguments}', f'cp {work}/{core} /out/']
    if not windows:
        script += [RUNNER.format(revision=revision, work=work)]
        mounts += mount(HERE, '/runner')
    script += [f'echo {revision} > /out/revision.txt']
    mounts += mount(VOLUME, '/work', kind='volume', readonly=False)
    mounts += mount(output, '/out', readonly=False)

    command = ['docker', 'run', '--rm', *mounts, IMAGE, 'bash', '-c', '\n'.join(script)]
    if subprocess.run(command).returncode != 0:
        raise Failure('the build failed')
    print(f'Built Beetle in {output}')


def main():
    parser = argparse.ArgumentParser(
        usage=argparse.SUPPRESS, description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', required=True, type=Path, metavar='CHECKOUT',
                        help="the fork's checkout, mounted read-only")
    parser.add_argument('--windows', action='store_true',
                        help='cross-build the Windows core instead of the Linux core and runner')
    parser.add_argument('--library', type=Path, metavar='DIR',
                        help="the emulator library's folder, in place of the checkout's "
                             'submodule')
    parser.add_argument('--output', type=Path, metavar='DIR',
                        help="the output folder, by default build/beetle or "
                             "build/beetle-windows in the demo's folder")
    parser.add_argument('--clean', action='store_true',
                        help="drop this build's folder in the work volume first, for a full "
                             'rebuild')
    options = parser.parse_args()
    try:
        build(options.source.resolve(), options.library and options.library.resolve(),
              options.windows, options.output, options.clean)
    except (Failure, OSError) as error:
        sys.exit(f'build.py: {error}')


if __name__ == '__main__':
    main()
