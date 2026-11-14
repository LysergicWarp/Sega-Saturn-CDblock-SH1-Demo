# CD-block SH-1 demo

Every Saturn has a third processor: a Hitachi SH-1 in the CD block, which runs the drive. This demo
puts it to work beside the two SH-2s. The CD block loads SH-1 code of our own from the MPEG card
slot or from a disc. The demo's firmware comes in through the card slot and lets the SH-2 upload
small SH-1 programs, called modules, and run jobs on them while the drive keeps reading.

It holds:

- A card firmware in SH-1 assembly, with a second version in C written to be read.
- Three modules for it: a PRS decompressor, a vertex transform and an XTEA cipher.
- An SH-2 client for the firmware's commands, and a dispatch that hands jobs to the slave SH-2 and
  to the SH-1 alike.
- Three discs that use them, and nine check programs.

The demo runs in an emulator: a
[fork of Beetle Saturn](https://github.com/LysergicWarp/beetle-saturn-libretro-cd-block-emulator)
with its CD block run by the
[CD-block emulator](https://github.com/LysergicWarp/Sega-Saturn-CDB-Emulator), which runs the stock
CD-block firmware. The firmware itself is described in the
[CD-block firmware documentation](https://github.com/LysergicWarp/Sega-Saturn-CDblock-Reverse-Engineering-Documentation).

Most of these files were produced while testing and developing the CD-block emulator, and are
preserved here for demo purposes.

No BIOS, CD-block ROM or disc image is included. You bring your own dumps and build the discs.

## The discs

| Disc | What it shows |
| --- | --- |
| `cd-block-sh1-demo` | A benchmark. One workload of disc decoding and 3D transforms runs four times: on the master SH-2 alone, with the slave, with the SH-1, and with both. |
| `card-library` | The three modules, each run from an upload and from the card ROM's library, with the load and job times. Every output is checked against the SH-2's own. |
| `scroller` | A picture wider than the screen, streamed off the disc in XTEA-encrypted strips. The pad's A button switches between showing the strips raw and having the SH-1 decrypt them. Until the first press it switches by itself every 10 s. |

All three need the card ROM.

## How it works

- The stock firmware has a slot for the MPEG card. Its card check and expansion loader load this
  firmware as they load a stock card's, and command 93 turns it on.
- The firmware adds three commands. AD gives its identity and protocol version, AB submits a job
  and AC reports the job's state and result.
- A module reaches the SH-1 as data. The SH-2 puts the module's package into a partition of the CD
  buffer through the data port, and a load job checks its CRC and installs it in the module slot.
- A job runs in passes from one of the stock firmware's tasks. The drive's tasks run between
  passes, so a long job never stops the drive.
- The card ROM also carries a library of up to five modules on fixed handles. A job on one of them
  needs no upload.
- With the assembly firmware a disc can check at boot that a library module is in the card ROM and
  whole. The C firmware has no room left for that check.
- A disc can carry SH-1 code too. The stock firmware loads an extension that the disc's header
  names. `src/disc-extensions/` has a small one, and `disc-extension-check` runs it.

The header comments of `src/card-firmwares/card-firmware-asm/card-firmware.s` and
`src/sh1-client/sh1-client.h` are the contract: the commands, the package format, the services a
module may call, and the limits.

## Layout

| Path | Contents |
| --- | --- |
| `src/card-firmwares/` | The card firmware in assembly and in C, and a minimal firmware that reports its identity and runs a ping job. |
| `src/modules-asm/` | The three modules. `transform-module.s` carries the notes on SH-1 assembly. |
| `src/sh1-client/` | The SH-2's side of the firmware's commands, with the transfers through the data port. |
| `src/dispatch/` | Jobs on the slave SH-2 and the SH-1 behind one interface. |
| `src/shell/` | The frame the scroller runs in: text rows, the pad and the toggle. |
| `src/demo-discs/` | The three discs. |
| `src/disc-extensions/` | An SH-1 extension loaded from a disc, used by one check program. |
| `tests/` | The check programs, each a disc that prints a line per step. |
| `tools/` | Build steps in Python: the card ROM, module packages, the benchmark's data, the cipher. |
| `docker/` | Builds Beetle Saturn with the emulator library and runs discs headless, with the headless runner's source. |

## Requirements

- Docker. The SH-2 and SH-1 code builds in a pinned [libyaul](https://github.com/yaul-org/libyaul)
  image, so no toolchain is installed on the host.
- Python 3 for `build.py`, `launch.py` and the scripts in `docker/`.
- Git, to clone the Beetle Saturn fork.
- An IPL BIOS dump and a CD-block ROM dump. The card firmware is written for CDB105 (SHA-1
  `eb8393058f324e922c11b43709b64fc6ca94ab86`).
- RetroArch, to run the discs with `launch.py`.

## Build

```
python build.py
```

- Output in `dist/`: `card-rom.bin`, the card ROM with the firmware and the modules' library, and
  the three discs, each a `.cue` with its `.iso`.
- `--firmware c` puts the C firmware in the card ROM instead. Its SH-1 compiler is built from
  `Dockerfile` the first time, which takes a while.
- `--clean` removes what `build.py` built, `dist/` included.
- `build.py` does not build the minimal firmware. Its Makefile's header says how, and `card-check`
  runs against its card ROM.

The discs' boot area comes from libyaul and contains Sega's security code, which the BIOS requires
of every disc. That is why the discs are built and not distributed.

## Run

Build the emulator core first, as [docker/README.md](docker/README.md) describes. Then:

```
python launch.py --bios bios.bin --cd-block cdb105.bin --card dist/card-rom.bin --disc dist/scroller.cue
```

- RetroArch loads the core with a profile of its own in `profile/`, so a RetroArch set up elsewhere
  keeps its settings.
- The core is the one `docker/` built into `build/`, and the RetroArch the one on the PATH.
  `--core` and `--retroarch` name others.
- `--region na|eu|jp` picks the console's region, `na` by default.
- `--transcript FILE` writes every CD-block command and its answer.
- `--hle` runs Beetle Saturn's own high-level CD block instead, to compare. It has no SH-1, so the
  card firmware does not run there.
- `launch.py --help` lists the rest.

`docker/check.py` runs a disc headless for a number of frames and saves screenshots.

## Check programs

Each directory under `tests/` is a disc that runs one area and prints `PASS` or `FAIL` per step.
`disc-extension-check` prints a table instead: per step, command 01's fourth word, E1's result
and the extension's two counters. `transfer-check` and `disc-extension-check` run without the card
ROM, and the others need it.

| Check | Covers |
| --- | --- |
| `card-check` | Loading the card firmware, turning it on, its identity and a ping job. |
| `transfer-check` | The client's DMA transfers through the data port. No card needed. |
| `module-check` | Loading the decompress and transform modules and running a job on each. |
| `library-check` | The boot-time check of each library handle, and a job on a checked module. |
| `lifecycle-check` | The firmware and a module across stops, resets, reloads and a cold start. |
| `dispatch-check` | One decode on each processor in turn, then on all three at once, and cache coherency between the SH-2s. |
| `disc-check` | A file decoded on the master and inside the CD block. |
| `cd-reset-check` | CD-block state across a host reset, a CD-block power cycle and a system reset. |
| `disc-extension-check` | An SH-1 extension loaded from the disc, across resets and repeated disc checks. |

Run `build.py` first, since several checks embed the modules. Then build a check in the same image,
from this directory:

```
docker run --rm -v "${PWD}:/work/project" -w /work/project/tests/card-check ijacquez/yaul@sha256:b6d272204767cd0a734139dd3a2ab298ef5912fb7b7224b78e6d9fac1a7825a6 make
```

The `.cue` and `.iso` land beside the check's source. `disc-extension-check` has its own script,
`python tests/disc-extension-check/craft.py`.

## Limits

- The firmware and everything it keeps share the CD block's expansion area of 20 KB with the module
  slot. A module's image and its variables fit the slot's 16 KB.
- One module is installed at a time, and one job runs at a time.
- The C firmware lacks the library check. The client works with both, and tells them apart by the
  protocol version AD gives: 2 for the assembly firmware, 1 for the C one.
- Not measured on hardware: how fast the SH-1 runs a job, idle or while the drive reads, what a
  data port word costs the SH-2, whether the CD block's interrupt reaches the SCU, and the card
  slot's electrical limits.
- Times on screen are emulated time. Beetle Saturn's bus sharing between the two SH-2s is
  approximate, with less slowdown than hardware, so runs where processors overlap look better than
  a console would show.
- In this mode the emulator has no CD audio, reads a changed disc as the old one, and leaves the CD
  block out of save states.

## License and credits

BSD 3-Clause, see [LICENSE](LICENSE).

- [libyaul](https://github.com/yaul-org/libyaul) (MIT): the Saturn SDK the SH-2 programs link
  against, its build image and its PRS compressor. `src/shell/` follows its examples.
- [Beetle Saturn](https://github.com/libretro/beetle-saturn-libretro) (GPL-2.0): the emulator the
  demo runs in, through the fork above. No core binary is included.
- XTEA, by David Wheeler and Roger Needham, 1997, public domain.
- The scroller's picture is "Glacial Mountains: Parallax Background" by
  [Vicente Nitti](https://vnitti.itch.io/), licensed under
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). See
  `src/demo-discs/scroller/art/Readme.txt`.
