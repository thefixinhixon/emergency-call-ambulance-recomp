#!/usr/bin/env python3
"""Build (or update) the HD texture pack for Emergency Call Ambulance.

    tools/build_texture_pack.py --upscaler /path/to/realesrgan-ncnn-vulkan [--run build/run]

1. Extracts every texture block from the ROM (tools/extract_texture_blocks.py), so the pack
   covers the whole game without playing it.
2. Upscales textures dumped while playing ("Dump textures" in the launcher), if any. These
   are upscaled one by one with wrap-aware padding, so tiling textures stay seamless, and
   the game prefers them over the whole-block versions.
3. Upscales the ROM blocks.
Every step skips work that is already done, so re-run it after dumping more textures.

Output: <run>/Textures/eca/*.png and <run>/Textures/eca/blocks/*.png. Turn on
"AI-upscaled textures" in the launcher's Video tab to use them.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(script, *args):
    cmd = [sys.executable, os.path.join(HERE, script), *args]
    print('>', ' '.join(cmd), flush=True)
    if subprocess.run(cmd).returncode != 0:
        sys.exit(f'{script} failed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--upscaler', required=True, help='path to realesrgan-ncnn-vulkan(.exe)')
    ap.add_argument('--run', default='build/run', help='the game\'s run directory')
    ap.add_argument('--rom', default='build/generated/rom', help='romtool output (created by the build)')
    ap.add_argument('--model', default='realesrgan-x4plus')
    ap.add_argument('--scale', default='4', help='HD size relative to the original (2, 3 or 4)')
    ap.add_argument('--detail', default='1.0', help='grain restoration strength (see upscale_textures.py)')
    args = ap.parse_args()

    tex = os.path.join(args.run, 'Textures')
    blocks_src = os.path.join(tex, 'eca_blocks_src')
    dumps = os.path.join(tex, 'eca_dump')
    pack = os.path.join(tex, 'eca')
    common = ['--upscaler', args.upscaler, '--model', args.model, '--scale', args.scale, '--detail', args.detail]

    run('extract_texture_blocks.py', '--rom', args.rom, '--out', blocks_src)
    if os.path.isdir(dumps) and any(f.endswith('.png') for f in os.listdir(dumps)):
        run('upscale_textures.py', *common, '--in', dumps, '--out', pack)
    else:
        print('no dumped textures yet; using ROM blocks only')
    run('upscale_textures.py', *common, '--in', blocks_src, '--out', os.path.join(pack, 'blocks'))
    print(f'texture pack ready in {pack}')


if __name__ == '__main__':
    main()
