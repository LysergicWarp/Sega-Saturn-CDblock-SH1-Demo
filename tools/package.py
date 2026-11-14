#!/usr/bin/env python3
"""Build module packages for the card firmware's module slot.

usage:
  package.py INPUT OUTPUT

INPUT is a module's image, linked to run in the module slot at 0x0907c000, header first. OUTPUT is
its package: the image with its last longword the CRC-32 of the bytes before it, as zlib computes
it. The image is checked for the header's magic, "DOSM", and for the size the header gives at +8,
a multiple of 4 from 36 bytes up to the slot's 16 KB.

examples:
  package.py decompress-module.bin decompress-package.bin
"""

import argparse
import struct
import sys
import zlib

SLOT_BYTES = 0x4000
MAGIC = 0x444F534D  # "DOSM"


class PackageError(Exception):
    pass


def package(image):
    """The package of a module image."""
    if len(image) < 36 or len(image) % 4:
        raise PackageError(f'a module image is a multiple of 4 bytes from 36, this is {len(image)}')
    if len(image) > SLOT_BYTES:
        raise PackageError(f'{len(image)} bytes, the slot holds {SLOT_BYTES}')
    magic, _, _, size = struct.unpack_from('>IHHI', image)
    if magic != MAGIC:
        raise PackageError(f'no module header: {magic:#010x} at +0, not {MAGIC:#010x}')
    if size != len(image):
        raise PackageError(f'the header gives {size} image bytes, the image is {len(image)}')
    return image[:-4] + struct.pack('>I', zlib.crc32(image[:-4]))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('input')
    parser.add_argument('output')
    args = parser.parse_args()
    with open(args.input, 'rb') as f:
        image = f.read()
    try:
        result = package(image)
    except PackageError as error:
        sys.exit(f'package.py: {error}')
    with open(args.output, 'wb') as f:
        f.write(result)
    print(f'{args.output}: {len(result)} bytes')


if __name__ == '__main__':
    main()
