#!/usr/bin/env python3
"""Encrypt a file for the cipher module: XTEA in counter mode, in whole 2048-byte sectors.

The module (src/modules-asm/cipher-module.s) decrypts a job's sectors with the same key stream. In
counter mode encrypting and decrypting are the same operation: each 8 bytes are XORed with XTEA's
encryption of a counter block, the job's stream number (AB's CR4) and the block's number from the
job's first sector, 256 a sector, both big-endian longwords. The key is the module's, the 16 bytes
"SH-1 demo cipher". The input is padded with zeros to whole sectors.

usage:
  cipher.py IN OUT --stream N

examples:
  cipher.py main.c build/main.xtea --stream 291
"""

import argparse
import struct
import sys

SECTOR_BYTES = 2048
KEY = struct.unpack('>4I', b'SH-1 demo cipher')
DELTA = 0x9E3779B9
MASK = 0xFFFFFFFF


def encipher(v0, v1, key=KEY):
    """XTEA's 32 cycles on the block (v0, v1)."""
    total = 0
    for _ in range(32):
        v0 = (v0 + ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (total + key[total & 3]))) & MASK
        total = (total + DELTA) & MASK
        v1 = (v1 + ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (total + key[(total >> 11) & 3]))) & MASK
    return v0, v1


def crypt(data, stream):
    """data padded to whole sectors, XORed with stream's key stream."""
    padded = data + bytes(-len(data) % SECTOR_BYTES)
    words = struct.unpack(f'>{len(padded) // 4}I', padded)
    out = []
    for block in range(len(words) // 2):
        k0, k1 = encipher(stream, block)
        out += (words[2 * block] ^ k0, words[2 * block + 1] ^ k1)
    return struct.pack(f'>{len(out)}I', *out)


def main():
    parser = argparse.ArgumentParser(
        usage=argparse.SUPPRESS, description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input')
    parser.add_argument('output')
    parser.add_argument('--stream', type=lambda text: int(text, 0), required=True,
                        help="the job's stream number, 0 to 65535")
    options = parser.parse_args()
    if not 0 <= options.stream <= 0xFFFF:
        sys.exit('cipher.py: the stream number is 0 to 65535')
    with open(options.input, 'rb') as f:
        data = f.read()
    with open(options.output, 'wb') as f:
        f.write(crypt(data, options.stream))


if __name__ == '__main__':
    main()
