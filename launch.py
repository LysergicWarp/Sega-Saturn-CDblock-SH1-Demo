#!/usr/bin/env python3
"""Run the Beetle Saturn fork in RetroArch, its CD block on the CD-block emulator library.

RetroArch loads the fork's core with the given IPL BIOS, CD-block ROM and disc image, and an MPEG
card ROM when one is given. The run keeps to a profile of its own: the system directory the core
reads its firmware from, the core's options, saves and states. The profile's settings are appended
to RetroArch's, which RetroArch then does not save, so a RetroArch set up elsewhere keeps its
settings and lends the run its video and input ones. The run starts muted.

Each run copies the IPL BIOS into the profile's system directory under the name the core reads for
the region, the CD-block ROM as cdblock.bin, which makes the core run the emulator library, and the
card ROM as mpeg_card.bin. It sets the core's region and card options, and sets the console's clock
from the host's, so the BIOS goes straight to the disc.

The core it runs is docker/build.py's, the --windows build on Windows and the Linux build
elsewhere, and the RetroArch the one on the PATH. --core names another core, --retroarch another
RetroArch.

usage:
  launch.py --bios BIOS --cd-block ROM --disc IMAGE [options] [-- RETROARCH-OPTION ...]

Options after -- go to RetroArch unchanged.

examples:
  launch.py --bios "Sega Saturn BIOS (USA).bin" --cd-block cdb105.bin --disc game.cue
  launch.py --bios bios.bin --cd-block cdb106.bin --disc game.cue --transcript commands.txt
  launch.py --bios bios.bin --cd-block cdb105.bin --card card-rom.bin --disc demo-disc.cue
  launch.py --bios bios.bin --cd-block cdb105.bin --disc game.cue -- --fullscreen --verbose
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROFILE = HERE / 'profile'

if sys.platform == 'win32':
    CORE = HERE / 'build' / 'beetle-windows' / 'mednafen_saturn_libretro.dll'
else:
    CORE = HERE / 'build' / 'beetle' / 'mednafen_saturn_libretro.so'

# Each region's option value and the IPL BIOS file the core reads for it.
REGIONS = {
    'na': ('North America', 'mpr-17933.bin'),
    'eu': ('Europe', 'mpr-17933.bin'),
    'jp': ('Japan', 'sega_101.bin'),
}

# The environment the fork's CD block reads its debug switches from.
LENIENT = 'CDB_LLE_LENIENT'
TRANSCRIPT = 'CDB_LLE_TRANSCRIPT'
TRANSCRIPT_DATA = 'CDB_LLE_TRANSCRIPT_DATA'


class Failure(Exception):
    pass


def quoted(value):
    return f'"{value}"'


def set_keys(text, values):
    """The settings text with the keys set, those missing added at its end."""
    lines = text.splitlines()
    missing = dict(values)
    for i, line in enumerate(lines):
        key, equals, _ = line.partition('=')
        key = key.strip()
        if equals and key in missing:
            lines[i] = f'{key} = {quoted(missing.pop(key))}'
    lines += [f'{key} = {quoted(value)}' for key, value in missing.items()]
    return '\n'.join(lines) + '\n'


def place(source, target):
    """Copies the file to target, or removes target when there is no file."""
    if source is None:
        target.unlink(missing_ok=True)
    else:
        shutil.copyfile(source, target)


def prepare(profile, bios, region, cd_block, card):
    """Sets the profile up for a run and returns the RetroArch settings to append."""
    system = profile / 'system'
    for directory in (system, profile / 'saves', profile / 'states'):
        directory.mkdir(parents=True, exist_ok=True)
    option, bios_name = REGIONS[region]
    for name in {name for _, name in REGIONS.values()}:
        place(bios if name == bios_name else None, system / name)
    place(cd_block, system / 'cdblock.bin')
    place(card, system / 'mpeg_card.bin')

    options = profile / 'core-options.cfg'
    text = options.read_text(encoding='utf-8') if options.exists() else ''
    text = set_keys(text, {
        'beetle_saturn_region': option,
        'beetle_saturn_mpeg_card': 'enabled' if card else 'disabled',
        'beetle_saturn_autortc': 'enabled',
        'beetle_saturn_autortc_lang': 'english',
    })
    with open(options, 'w', encoding='utf-8', newline='\n') as file:
        file.write(text)

    settings = profile / 'retroarch.cfg'
    values = {
        'system_directory': system,
        'savefile_directory': profile / 'saves',
        'savestate_directory': profile / 'states',
        'global_core_options': 'true',
        'core_options_path': options,
        'game_specific_options': 'false',
        'config_save_on_exit': 'false',
        'history_list_enable': 'false',
        'content_runtime_log': 'false',
        'audio_mute_enable': 'true',
    }
    with open(settings, 'w', encoding='utf-8', newline='\n') as file:
        file.write(''.join(f'{key} = {quoted(value)}\n' for key, value in values.items()))
    return settings


def main():
    arguments = sys.argv[1:]
    passed = []
    if '--' in arguments:
        at = arguments.index('--')
        arguments, passed = arguments[:at], arguments[at + 1:]
    parser = argparse.ArgumentParser(
        usage=argparse.SUPPRESS, description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bios', required=True, type=Path, metavar='BIOS',
                        help='the IPL BIOS image')
    parser.add_argument('--cd-block', required=True, type=Path, metavar='ROM',
                        help='the CD-block ROM image')
    parser.add_argument('--disc', required=True, type=Path, metavar='IMAGE',
                        help='the disc image, in any format Beetle Saturn loads')
    parser.add_argument('--card', type=Path, metavar='ROM',
                        help="an MPEG card ROM, put in the emulator's card slot")
    parser.add_argument('--region', choices=sorted(REGIONS), default='na',
                        help="the console's region, and so which BIOS name the core reads: na "
                             'by default')
    parser.add_argument('--core', type=Path, default=CORE, metavar='FILE',
                        help="the core to run, by default docker/'s build in build/")
    parser.add_argument('--retroarch', type=Path, metavar='FILE',
                        help='the RetroArch to run, by default the one on the PATH')
    parser.add_argument('--profile', type=Path, default=PROFILE, metavar='DIR',
                        help='the profile directory, by default profile/ beside this script')
    parser.add_argument('--lenient', action='store_true',
                        help="report the emulator's strict checks in the core's log instead of "
                             'stopping the CD block')
    parser.add_argument('--transcript', type=Path, metavar='FILE',
                        help="write the host's CD block commands and responses to FILE")
    parser.add_argument('--transcript-data', action='store_true',
                        help='also write every data port word the host reads to the transcript')
    parser.add_argument('--hle', action='store_true',
                        help="run Beetle Saturn's own high-level CD block instead, to compare")
    options = parser.parse_args(arguments)
    if options.hle and (options.card or options.lenient or options.transcript):
        parser.error('--card, --lenient and --transcript apply to the emulator, not to --hle')
    if options.transcript_data and not options.transcript:
        parser.error('--transcript-data needs --transcript')
    try:
        for path in (options.bios, options.cd_block, options.disc, options.card):
            if path is not None and not path.is_file():
                raise Failure(f'no file {path}')
        if not options.core.is_file():
            raise Failure(f'no core at {options.core}: build the fork, or name a core with --core')
        if options.retroarch is None:
            found = shutil.which('retroarch')
            if found is None:
                raise Failure('no retroarch on the PATH: name one with --retroarch')
            options.retroarch = Path(found)
        if not options.retroarch.is_file():
            raise Failure(f'no RetroArch at {options.retroarch}: name one with --retroarch')
        settings = prepare(options.profile.resolve(), options.bios, options.region,
                           None if options.hle else options.cd_block, options.card)
    except (Failure, OSError) as error:
        sys.exit(f'launch.py: {error}')

    environment = dict(os.environ)
    for name in (LENIENT, TRANSCRIPT, TRANSCRIPT_DATA):
        environment.pop(name, None)
    if options.lenient:
        environment[LENIENT] = '1'
    if options.transcript:
        environment[TRANSCRIPT] = str(options.transcript.resolve())
        if options.transcript_data:
            environment[TRANSCRIPT_DATA] = '1'

    command = [str(options.retroarch), f'--appendconfig={settings}', '-L',
               str(options.core.resolve()), str(options.disc.resolve()), *passed]
    try:
        if os.name == 'posix':
            os.execve(command[0], command, environment)     # RetroArch takes this process
        result = subprocess.run(command, env=environment, cwd=options.retroarch.parent)
    except OSError as error:
        sys.exit(f'launch.py: {error}')
    sys.exit(result.returncode)


if __name__ == '__main__':
    main()
