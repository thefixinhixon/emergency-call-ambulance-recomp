Emergency Call Ambulance (Sega Model 3, 1999) for Linux and Windows
https://github.com/thefixinhixon/emergency-call-ambulance-recomp

This prebuilt package contains NO game code and NO ROMs. You need your own "eca" ROM set.

The prebuilt game runs the PowerPC code on Supermodel's interpreter. The recompiled
(native) PowerPC code is a translation of the game's ROM, so it can't be distributed;
build from source with your ROM set to get it (see the repository README). Everything
else is the same: launcher, controller profile, renderer and HD texture support. The
interpreter runs the game at full speed on any recent x86-64 CPU.


RUNNING
  1. Copy your eca.zip into ROMs/. It must be the "eca" set (the World version) as
     MAME / Supermodel name it.
     If your zip holds eca.zip inside it, extract the inner one, or let romtool do it
     (on Windows the commands below use tools\romtool.exe and python):
         tools/romtool Config/Games.xml your.zip eca rom
     which writes rom/eca.zip.
  2. Start eca (Windows: eca.exe) with no arguments. The launcher opens: pick
     settings, then Launch. "eca ROMs/eca.zip" skips the launcher.

  Linux needs glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 or later) and
  OpenGL with GLU, which most desktops already have; otherwise
      Debian/Ubuntu: sudo apt install libglu1-mesa
      Fedora:        sudo dnf install mesa-libGLU
      Arch:          sudo pacman -S glu
  Windows needs nothing extra.


CONTROLS
  Xbox-layout pad:  Back = coin, Start = start, left stick = steering,
                    RT = accelerator, LT = brake, RB / LB = shift up / down, Y = view
  Keyboard:         3 = coin, 1 = start, arrows = steer / accelerate / brake,
                    Y / H = shift up / down, A = view
  Rebind anything on the launcher's Controls tab. Settings are saved in
  Config/Supermodel.ini, section [ eca ].


HD TEXTURES (optional)
  The launcher's Video tab has an "AI-upscaled textures" checkbox. It uses a texture
  pack you make yourself from your ROM with Real-ESRGAN (realesrgan-ncnn-vulkan v0.2.5.0,
  any Vulkan GPU) and Python 3 with Pillow and numpy:
      tools/romtool Config/Games.xml ROMs/eca.zip eca rom
      python3 tools/build_texture_pack.py --upscaler /path/to/realesrgan-ncnn-vulkan --run . --rom rom
  The pack is about 1.5 GB and takes a while to make; use --scale 2 for smaller GPUs.


LICENSE
  GPL-3.0 (see LICENSE). Uses Supermodel (https://github.com/trzy/Supermodel), GPL-3.0.
