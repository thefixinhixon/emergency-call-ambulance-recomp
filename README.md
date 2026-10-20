# Emergency Call Ambulance: static recompilation

A native x86_64 build of Sega's *Emergency Call Ambulance* (Model 3 Step 2.1, 1999) for
Linux and Windows. The game's PowerPC 603e code is translated ahead of time into C++
and compiled with your system compiler. The rest of the Model 3 hardware (Real3D Pro-1000
GPU, tilegen, SCSP sound with its 68000, I/O, inputs) comes from
[Supermodel](https://github.com/trzy/Supermodel), which is included as a git submodule.

No game code is included. The recompiled C++ is generated at build time from **your own**
ROM set and must not be redistributed.

## How it works

```
roms/eca.zip ─► romtool ─► crom.bin (big-endian) ─► ppcrecomp ─► generated/*.cpp ─┐
                                ▲                       ▲                          │
              config/eca.cfg ───┘   config/eca_seeds.txt┘                          ▼
                                                    eca = Supermodel devices + hybrid PPC core
```

* **romtool** loads the set through Supermodel's loader. It handles wrapper zips that
  contain `eca.zip`, writes the fixed CROM as a big-endian image, and writes a
  `Games.xml` filtered to `eca`, because merged sets otherwise load `ecaj`.
* **ppcrecomp** follows control flow from the configured entry points and seeds and
  emits one C++ function per *unit*: the code reachable from an entry point without a
  call. Common integer, branch, load/store and FP load/store instructions are inlined.
  Everything else calls Supermodel's interpreter handler for that one instruction.
* At boot the game copies its main program from CROM `0xFFC80000` to RAM `0`, and most
  of the game runs from RAM. `config/eca.cfg` describes that copy so the RAM code is
  recompiled from the ROM too. At run time each RAM unit is checked against RAM before
  it runs, and any store that hits a line holding recompiled code triggers a re-check.
* **runtime/ppc_core.cpp** replaces `ppc_execute()`. A dispatcher runs recompiled units
  and interprets anything it has no unit for. Every block checks that it can finish
  without crossing the cycle budget or a decrementer tick, and hands that block to the
  interpreter otherwise. The result is instruction-exact with Supermodel's interpreter,
  which the RAM test below checks.

## Prebuilt releases

The [releases page](https://github.com/thefixinhixon/emergency-call-ambulance-recomp/releases)
has Linux and Windows packages, built by `.github/workflows/release.yml` when a `v*` tag is
pushed. They are built without a ROM set (`-DECA_RECOMPILE=OFF`), so they hold no game code
and run the PowerPC code on Supermodel's interpreter. Everything else is the same. Put your
`eca.zip` in `ROMs/` and run `eca`; `packaging/README.txt` is the README shipped with them.
`tools/package_release.sh` makes the same packages from a local build, and refuses a build
that contains recompiled code.

## Building

Requirements: CMake ≥ 3.24, Ninja (optional), a C++20 compiler, SDL2 and OpenGL/GLU
development files. SDL2_net and zlib are downloaded when they are not installed.

```sh
git clone --recursive <this repo>
cp /path/to/your/eca.zip roms/eca.zip      # the set, or a zip that contains eca.zip
cmake -S . -B build -G Ninja
cmake --build build
cd build/run && ./eca eca.zip
```

Running `./eca` with **no arguments** opens the game's launcher (`runtime/launcher.cpp`):

| Tab | Settings |
|---|---|
| Video | display mode, resolution (presets, your desktop size, or custom), widescreen, stretch, 3D engine, supersampling, 2D upscaling, CRT colours, vsync, 57.524 Hz mode |
| Audio | sound on/off, volume, balance, channels, stereo swap, legacy DSP |
| Controls | detected controllers, rebind every game input (Set / Add / Clear), reset to pad defaults, trigger pedals, steering range and dead zone, force feedback |
| System | threading, recompiled vs. interpreter CPU core (debugging, not saved), reset NVRAM |

Settings are saved to the `[ eca ]` section of `Config/Supermodel.ini`, which overrides
the global section for this game. Launch saves and starts the game; the launcher works
with a controller (A selects, and Launch has focus when it opens). Starting with
arguments (`./eca eca.zip ...`) skips the launcher and uses the saved settings plus any
command line options.

The playable build is `build/run/` (binary, `Config/`, `Assets/`, `NVRAM/`, and the
extracted `eca.zip`). All of Supermodel's command line options work, for example
`-fullscreen`, `-res=1920,1080`, `-wide-screen` and `-config-inputs`.

### Controls

The build adds an `[ eca ]` profile for Xbox-layout pads to `run/Config/Supermodel.ini`
(from `config/eca_gamepad.ini`; existing settings are left alone):

| Pad | Game | Keyboard |
|---|---|---|
| Back | Coin | 3 |
| Start | Start | 1 |
| Left stick | Steering | ← → |
| Right trigger | Accelerator | ↑ |
| Left trigger | Brake | ↓ |
| RB / LB | Shift up / down | Y / H |
| Y | View change | A |

Remap them on the launcher's Controls tab, with `./eca eca.zip -config-inputs`, or by editing
the `[ eca ]` section.

### Windows

Cross-compile from Linux with MinGW-w64. The generators must run on the host, so build
them natively first:

```sh
cmake -S . -B build -G Ninja && cmake --build build --target m68kmake romtool ppcrecomp
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DM68KMAKE_EXECUTABLE=$PWD/build/m68kmake \
  -DECA_HOST_ROMTOOL=$PWD/build/tools/romtool/romtool \
  -DECA_HOST_PPCRECOMP=$PWD/build/tools/ppcrecomp/ppcrecomp
cmake --build build-win --target eca        # -> build-win/run/eca.exe (static, no DLLs)
```

Native MSVC builds are not tested yet.

## HD textures (AI upscaling)

Optional 4x texture replacements made with [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)
(download a `realesrgan-ncnn-vulkan` build for your OS from the v0.2.5.0 release; it runs on any
Vulkan GPU). Python 3 with Pillow and numpy is needed to build the pack:

```sh
tools/build_texture_pack.py --upscaler /path/to/realesrgan-ncnn-vulkan
```

Then tick **AI-upscaled textures** on the launcher's Video tab (New3D engine only).

How the pack is made and used:

* **Whole game, no playing needed.** The game fills texture memory with 256x256 blocks that
  it copies from VROM or banked CROM, using a table in the program ROM (`0xFF838080`). The
  extractor walks that table, rebuilds all 228 blocks exactly as the Real3D stores them, and
  writes each one in the four colour formats that cover about 96% of what the game draws.
  A block's format isn't stored in the ROM, because each polygon picks it.
* **Seamless where it matters.** Textures dumped while playing (launcher: System > *Dump
  textures while playing*) are upscaled one at a time, padded according to how they wrap,
  so repeating surfaces like roads and walls don't show seams. These take priority. Every
  other texture is cut from its upscaled block.
* **Grain kept.** Real-ESRGAN smooths away fine noise such as asphalt and grass. The script
  back-projects onto the original so that grain survives while edges stay sharp
  (`--detail 0` turns this off).
* **At run time** (`runtime/hdtex.cpp`, spliced into Supermodel's New3D renderer at build
  time by `cmake/patch_supermodel.cmake`), each block load is recognised by a hash of its
  texels. Each draw binds its replacement, sampled at the Real3D's own level of detail.
  Block images decode on a background thread, so a stage load never stalls; a texture
  shows its original version for the first frame or two.

The full pack is about 1.5 GB on disk. A scene uses up to 64 blocks, roughly 0.5 GB of VRAM at 4x;
use `--scale 2` for smaller GPUs.

## Improving coverage

PCs that the dispatcher had to interpret are written to `ppc_misses.txt` when the game
exits. Feed them back in and rebuild. A seed that lands inside code another unit already
covers (typically the point an interrupt returns to) only splits a block in that unit,
so adding misses does not duplicate code:

```sh
tools/update_seeds.py config/eca_seeds.txt build/run/ppc_misses.txt
cmake --build build
```

Ghidra finds code that is only reached through function pointers and switch tables
before the game ever runs it. It needs Ghidra 12 and a JDK 21+:

```sh
JAVA_HOME=/path/to/jdk-21 tools/ghidra/analyze.sh /path/to/ghidra \
  build/generated/rom/crom.bin config/eca.cfg analysis/ghidra_seeds.txt config/eca_seeds.txt
tools/update_seeds.py config/eca_seeds.txt analysis/ghidra_seeds.txt
```

This leaves a Ghidra project in `analysis/ghidra/`, with the RAM code image and MMIO
regions mapped, for interactive work in Ghidra or through
[REA](https://github.com/morluto/rea). Install REA only from the official sources
(`morluto/rea`, npm `rea-agents`); look-alike copies have shipped malware.

## Testing

```sh
tests/compare_ram.sh build 15                         # Linux
EXE="wine eca.exe" tests/compare_ram.sh build-win 15  # Windows build under Wine
```

The test runs 15 emulated seconds twice with Supermodel's stock interpreter and once with
recompiled code, then compares the RAM dumps. Supermodel is not bit-repeatable from run
to run (its RTC reads the host clock), so a byte only counts as a mismatch when it
differs from both stock runs.

## Runtime switches

| Variable | Effect |
|---|---|
| `ECA_PPC_MODE=interp` | dispatcher only, no recompiled code |
| `ECA_PPC_MODE=stock` | Supermodel's original `ppc_execute()` |
| `ECA_PPC_STATS=1` | print native/interpreted counts and PPC host time on exit |
| `ECA_PPC_MISSLOG=<file>` | where to write the miss log |
| `ECA_EXIT_AFTER=<sec>` | quit after this much emulated time |
| `ECA_HD_TEXTURES=1`, `ECA_DUMP_TEXTURES=1` | override the launcher's texture settings |
| `ECA_HDTEX_VERBOSE=1` | report each texture block load and whether the pack has it |
| `ECA_LOG_TEXPORT=<file>` | log Real3D texture loads (how the ROM texture table was found) |
| `ECA_LAUNCHER_TAB=<tab>` | open the launcher on a tab (Video, Audio, Controls, System) |
| `ECA_LAUNCHER_AUTO=launch\|save` | press Launch, or save and quit, without user input (tests) |

## License

GPL-3.0-or-later, the same as Supermodel. See `LICENSE`.
