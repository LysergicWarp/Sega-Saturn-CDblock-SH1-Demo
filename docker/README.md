# Building and running Beetle Saturn

The demo runs in a
[fork of Beetle Saturn](https://github.com/LysergicWarp/beetle-saturn-libretro-cd-block-emulator)
built with the [CD-block emulator](https://github.com/LysergicWarp/Sega-Saturn-CDB-Emulator). The
scripts here build that core in Docker and run discs on it: a Windows core for RetroArch, and a
Linux core with a headless runner. They need Docker and Python 3, and no toolchain on the host.

The core runs the CD block on the emulator library when the CD-block ROM is in its system directory
as `cdblock.bin`, and Beetle Saturn's own high-level CD block otherwise. Its MPEG card option puts
`mpeg_card.bin` in the emulator's card slot. `launch.py` and `check.py` set both up.

## Building

Clone the fork with its submodule, the emulator library:

```
git clone --recurse-submodules https://github.com/LysergicWarp/beetle-saturn-libretro-cd-block-emulator beetle-saturn
```

Then, from the demo's folder:

```
python docker/build.py --source path/to/beetle-saturn --windows
python docker/build.py --source path/to/beetle-saturn
```

- Output in `build/beetle-windows` and `build/beetle` in the demo's folder. `--output` names
  another place.
- The image comes from the `Dockerfile` here: Debian 13 pinned by digest, GCC 14, and MinGW-w64
  GCC 14 with posix threads at pinned versions.
- The checkout mounts read-only and is copied into a Docker volume without its Git metadata. The
  core is built with `HAVE_CDB_LLE=1`.
- The library is the checkout's `deps/cd-block-sh1-emulator`. `--library` names another copy of
  the emulator, whose `include/` and `src/` are built instead.
- Rebuilds are incremental. `--clean` drops the volume first.
- With `--windows` it writes `mednafen_saturn_libretro.dll` with LTO on, which Beetle Saturn's
  Makefile leaves off for Windows. The core needs `libwinpthread-1.dll`, which RetroArch's Windows
  builds ship beside `retroarch.exe`.
- Without it, it writes `mednafen_saturn_libretro.so` with LTO, and `beetle-runner`, the headless
  runner built from `beetle-runner.c` here.
- Both write `revision.txt`, the checkout's commit, and report local changes in the checkout.

## Running

```
python launch.py --bios bios.bin --cd-block cdb105.bin --card dist/card-rom.bin --disc dist/scroller.cue
```

- RetroArch loads the core: the Windows core from `build/beetle-windows` on Windows, the Linux
  core from `build/beetle` elsewhere. `--core` names another.
- The RetroArch is the one on the PATH. `--retroarch` names another.
- The run keeps to `profile/` in the demo's folder, or the folder `--profile` names: the system
  directory with the BIOS, `cdblock.bin` and `mpeg_card.bin`, the core's options, saves and states.
- The profile's settings are appended to RetroArch's with saving on exit off, so RetroArch's own
  settings stay as they are. The run starts muted.
- `--region na|eu|jp` picks the console's region and the BIOS name the core reads, `na` by default.
- `--lenient` logs the emulator's strict checks instead of stopping the CD block.
- `--transcript FILE` writes the host's commands and the responses it reads. `--transcript-data`
  adds every data port word the host reads.
- `--hle` runs Beetle Saturn's own CD block instead, to compare.
- Options after `--` go to RetroArch unchanged.

## Headless runs

```
python docker/check.py --disc dist/scroller.cue --card dist/card-rom.bin --bios bios.bin --cd-block cdb105.bin --frames 3600 --screenshots 600 3600
```

- Runs the Linux core in the build image for a number of frames, 1800 by default, and times the
  run.
- The core and the runner come from `build/beetle`, or the folder `--build` names. `--card` is
  optional.
- `--region`, `--hle` and `--lenient` match the launcher's options. `--transcript` writes
  `transcript.txt`, and `--transcript-data` adds the data port words to it.
- Output in `build/checks/` under the disc's name, or the folder `--output` names. The folder is
  emptied first. It gets the runner's `result.json`, `log.txt` with the core's log and the elapsed
  time, `transcript.txt` with `--transcript`, and each frame `--screenshots` names as a PNG file.
