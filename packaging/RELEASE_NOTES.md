Prebuilt *Emergency Call Ambulance* for x86-64 Linux and Windows, with the game's own launcher, an Xbox-layout controller profile, and support for AI-upscaled HD textures.

**No game code or ROMs are included.** You need your own `eca` ROM set: put `eca.zip` in `ROMs/` and run `eca` (or `eca.exe`). `README.txt` in each package has the details.

The prebuilt packages run the PowerPC code on Supermodel's interpreter, which reaches full speed on any recent CPU. The recompiled native code is a translation of the game's ROM, so it can't be distributed. To get it, build from source with your ROM set (see the repository README).

| Package | Notes |
|---|---|
| `…-windows-x86_64.zip` | Self-contained, no DLLs needed |
| `…-linux-x86_64.tar.gz` | Needs SDL2, OpenGL and GLU from your distribution (glibc 2.35+, e.g. Ubuntu 22.04 or newer) |

Each package also has `tools/romtool` plus the texture scripts, for making the optional HD texture pack from your ROM with Real-ESRGAN.
