#!/usr/bin/env python3
"""Add a library of module packages to a card ROM, for the card firmware's fixed handles.

usage:
  library.py ROM PACKAGE...

ROM is a card ROM from payload.py card, changed in place. The library's table goes at 0x8000, past
the most the encoded firmware takes: 0x444f534c ("DOSL"), the count, then each package's offset in
the ROM, all big-endian longwords. The packages follow it, each from a 16-byte boundary. The first
package is on handle 251, the next on 252 and so on, five at most. Each package is checked for its
header's magic, "DOSM", and for the image size its header gives, and the ROM for zeros where the
library goes.

examples:
  library.py card-rom.bin decompress-package.bin transform-package.bin
"""

import argparse
import struct
import sys

TABLE = 0x8000
MAGIC = 0x444F534C  # "DOSL"
MODULE_MAGIC = 0x444F534D  # "DOSM"
HANDLE_FIRST = 251
HANDLES = 5


class LibraryError(Exception):
    pass


def library(rom, packages):
    """rom with the library of packages added."""
    if not 1 <= len(packages) <= HANDLES:
        raise LibraryError(
            f'1 to {HANDLES} packages, for handles {HANDLE_FIRST} up, not {len(packages)}')
    offsets = []
    at = (TABLE + 8 + 4 * len(packages) + 15) & ~15
    for name, data in packages:
        magic, _, _, size = struct.unpack_from('>IHHI', data)
        if magic != MODULE_MAGIC or size != len(data):
            raise LibraryError(f'{name}: not a package, or its header does not give its size')
        offsets.append(at)
        at = (at + len(data) + 15) & ~15
    if at > len(rom):
        raise LibraryError(f'the library ends at {at:#x}, the ROM at {len(rom):#x}')
    if any(rom[TABLE:at]):
        raise LibraryError(f'the ROM is not zero from {TABLE:#x} to {at:#x}')
    rom = bytearray(rom)
    struct.pack_into(f'>II{len(offsets)}I', rom, TABLE, MAGIC, len(offsets), *offsets)
    for (_, data), offset in zip(packages, offsets):
        rom[offset:offset + len(data)] = data
    return bytes(rom)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('rom')
    parser.add_argument('packages', nargs='+')
    args = parser.parse_args()
    with open(args.rom, 'rb') as f:
        rom = f.read()
    packages = []
    for name in args.packages:
        with open(name, 'rb') as f:
            packages.append((name, f.read()))
    try:
        result = library(rom, packages)
    except LibraryError as error:
        sys.exit(f'library.py: {error}')
    with open(args.rom, 'wb') as f:
        f.write(result)
    for handle, (name, data) in enumerate(packages, HANDLE_FIRST):
        print(f'{args.rom}: handle {handle}, {name}, {len(data)} bytes')


if __name__ == '__main__':
    main()
