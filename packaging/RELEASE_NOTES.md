Prebuilt *Emergency Call Ambulance* for x86-64 Linux and Windows, with the game's own launcher, an Xbox-layout controller profile, and support for AI-upscaled HD textures.

**New in this release:**

- **Force-feedback setup.** The game needs an optional drive-board ROM (`epr-19338a.bin`) for force feedback. The launcher now shows whether it's present and can import it into `eca.zip` straight from a `scud.zip` you place next to it (System tab → Import FFB ROM), with checksum validation.
- **Steering wheels with 9+ axes.** Wheels like the Logitech G29 in separate-pedal mode report the brake past Supermodel's old 8-axis cap and couldn't bind it. The cap is raised to 12 axes, so gas, brake, wheel, and shifter all bind from the Controls tab.
- **Pedal calibration.** The Controls tab has pedal Y min/off/max calibration, including the values for combined-pedal setups where gas and brake share one axis (Min −16000, Off 0, Max 16000), plus a reset to defaults.

**No game code or ROMs are included.** You need your own `eca` ROM set: put `eca.zip` in `ROMs/` and run `eca` (or `eca.exe`). `README.txt` in each package has the details.

The prebuilt packages run the PowerPC code on Supermodel's interpreter, which reaches full speed on any recent CPU. The recompiled native code is a translation of the game's ROM, so it can't be distributed. To get it, build from source with your ROM set (see the repository README).

| Package | Notes |
|---|---|
| `…-windows-x86_64.zip` | Self-contained, no DLLs needed |
| `…-linux-x86_64.tar.gz` | Needs glibc 2.35+ (Ubuntu 22.04 or newer) and OpenGL/GLU; SDL2 is built in |

Each package also has `tools/romtool` plus the texture scripts, for making the optional HD texture pack from your ROM with Real-ESRGAN.
