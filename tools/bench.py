#!/usr/bin/env python3
"""Make the disc decode benchmark's data: a text file cut in chunks, each its own PRS stream.

usage:
  bench.py chunks DIRECTORY
  bench.py pack DIRECTORY STREAM TABLE

chunks writes the text, the same each run, as DIRECTORY/chunk-NNN.bin, CHUNK_BYTES each. bcl_prs,
in the yaul image, then compresses each into chunk-NNN.prs beside it. pack lays the streams out as
one disc file, STREAM, each from a sector's start and padded with zeros to a whole sector, and
writes TABLE for the demo: big-endian longwords, the chunk count, then for each chunk its first
sector in STREAM, its stream's bytes, its output's bytes and the output's checksum. The checksum
runs over the output's big-endian longwords: sum = sum * 31 + longword, modulo 2^32.

examples:
  bench.py chunks src/demo-discs/demo/build/bench
  bench.py pack src/demo-discs/demo/build/bench src/demo-discs/demo/build/bench.prs
      src/demo-discs/demo/build/bench-table.bin
"""

import argparse
import random
import struct
import sys
from pathlib import Path

SECTOR_BYTES = 2048
CHUNKS = 180
CHUNK_BYTES = 32768     # 16 sectors of output, a multiple of 4 for the checksum
STREAM_SECTORS_MOST = 8     # CHUNK_INPUT_MOST in src/demo-discs/demo/benchmark.c

# Words of the kind the module check's text has, so the text compresses as that one does.
WORDS = ('host card firmware filter partition sector slot buffer copy pass decoder output job '
         'stream drive frame track module handle transfer port command flag status the a of to '
         'and in on read write move get put play seek').split()


class BenchError(Exception):
    pass


def text(seed):
    """CHUNKS * CHUNK_BYTES of words in lines, from a seeded generator."""
    rng = random.Random(seed)
    out = bytearray()
    while len(out) < CHUNKS * CHUNK_BYTES:
        words = [rng.choice(WORDS) for _ in range(rng.randint(1, 9))]
        line = ' '.join(words)
        out += (line + rng.choice(('.', '', '. ')) + '\n').encode('ascii')
    return bytes(out[:CHUNKS * CHUNK_BYTES])


def checksum(data):
    total = 0
    for (word,) in struct.iter_unpack('>I', data):
        total = (total * 31 + word) & 0xFFFFFFFF
    return total


def chunks(directory):
    directory.mkdir(parents=True, exist_ok=True)
    data = text(0x5A7E)
    for index in range(CHUNKS):
        part = data[index * CHUNK_BYTES:(index + 1) * CHUNK_BYTES]
        (directory / f'chunk-{index:03}.bin').write_bytes(part)
    print(f'{directory}: {CHUNKS} chunks of {CHUNK_BYTES} bytes')


def pack(directory, stream_path, table_path):
    stream = bytearray()
    table = [struct.pack('>I', CHUNKS)]
    for index in range(CHUNKS):
        output = (directory / f'chunk-{index:03}.bin').read_bytes()
        compressed = (directory / f'chunk-{index:03}.prs').read_bytes()
        if len(compressed) > STREAM_SECTORS_MOST * SECTOR_BYTES:
            raise BenchError(f'chunk {index}: {len(compressed)} bytes, the demo takes '
                             f'{STREAM_SECTORS_MOST} sectors')
        first = len(stream) // SECTOR_BYTES
        stream += compressed + bytes(-len(compressed) % SECTOR_BYTES)
        table.append(struct.pack('>IIII', first, len(compressed), len(output), checksum(output)))
    Path(stream_path).write_bytes(stream)
    Path(table_path).write_bytes(b''.join(table))
    sectors = len(stream) // SECTOR_BYTES
    print(f'{stream_path}: {sectors} sectors, {CHUNKS * CHUNK_BYTES / len(stream):.2f} to 1')


def main():
    parser = argparse.ArgumentParser(
        usage=argparse.SUPPRESS, description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('chunks').add_argument('directory', type=Path)
    packing = commands.add_parser('pack')
    packing.add_argument('directory', type=Path)
    packing.add_argument('stream')
    packing.add_argument('table')
    options = parser.parse_args()
    try:
        if options.command == 'chunks':
            chunks(options.directory)
        else:
            pack(options.directory, options.stream, options.table)
    except (BenchError, OSError) as error:
        sys.exit(f'bench.py: {error}')


if __name__ == '__main__':
    main()
