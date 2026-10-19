#!/usr/bin/env python3
"""Build an HD texture pack from dumped textures with Real-ESRGAN.

    tools/upscale_textures.py --upscaler /path/to/realesrgan-ncnn-vulkan \\
        [--in build/run/Textures/eca_dump] [--out build/run/Textures/eca] \\
        [--model realesrgan-x4plus] [--scale 4] [--force]

Input is what the game writes with "Dump textures" on (one PNG per unique texture plus
wrap.txt). For each texture:
  * colour under transparent pixels is filled in from the opaque neighbours, so the
    network does not smear the transparency key colour into the edges;
  * the image is padded with wrapped (repeating) or mirrored copies of itself, matching
    how the game samples it, so tiling textures stay seamless after upscaling;
  * RGB goes through Real-ESRGAN, alpha is resized with Lanczos (1-bit alpha is
    re-thresholded so cut-outs stay crisp), and the padding is cropped off;
  * Real-ESRGAN treats fine grain (asphalt, grass, concrete) as noise and smooths it
    away, so the result is back-projected onto the original: wherever the downscaled HD
    texture differs from the original, the difference is added back at full size. This
    keeps the sharper edges but restores the original's grain. --detail scales it.
Existing outputs are skipped unless --force is given, so the script can be re-run after
dumping more of the game.

Real-ESRGAN ncnn/Vulkan builds (Linux, Windows, macOS; models included):
https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit('upscale_textures.py needs Pillow and numpy (pip install pillow numpy)')

MODEL_SCALE = {'realesrgan-x4plus': 4, 'realesrgan-x4plus-anime': 4, 'realesr-animevideov3': 4}


def read_wrap_modes(dump_dir):
    modes = {}
    try:
        with open(os.path.join(dump_dir, 'wrap.txt')) as f:
            for line in f:
                parts = line.split()
                if len(parts) == 3:
                    modes[parts[0]] = (int(parts[1]), int(parts[2]))
    except FileNotFoundError:
        pass
    return modes


def bleed_transparent(rgba, iterations=16):
    """Fill RGB under fully transparent pixels with the average of opaque neighbours."""
    rgb = rgba[..., :3].astype(np.float32)
    known = rgba[..., 3] > 0
    if known.all() or not known.any():
        return rgba[..., :3]
    for _ in range(iterations):
        if known.all():
            break
        acc = np.zeros_like(rgb)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            k = np.roll(np.roll(known, dy, 0), dx, 1)
            acc += np.roll(np.roll(rgb, dy, 0), dx, 1) * k[..., None]
            cnt += k
        grow = (~known) & (cnt > 0)
        rgb[grow] = acc[grow] / cnt[grow][:, None]
        known = known | grow
    return np.clip(rgb, 0, 255).astype(np.uint8)


def back_project(up, orig, strength, iterations=3):
    """Make the HD image agree with the original when downscaled (restores lost grain)."""
    if strength <= 0:
        return up
    H, W = up.shape[:2]
    h, w = orig.shape[:2]
    out = up.astype(np.float32)
    o = orig.astype(np.float32)
    for _ in range(iterations):
        down = np.asarray(Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).resize((w, h), Image.BOX), np.float32)
        err = o - down
        err_up = np.stack([np.asarray(Image.fromarray(err[..., c], 'F').resize((W, H), Image.BICUBIC)) for c in range(3)], -1)
        out += strength * err_up
    return np.clip(out, 0, 255).astype(np.uint8)


def pad_mode(wrap):
    # New3D wrap modes: 0/1 repeat, 2/3 mirror; None for ROM blocks, whose neighbours are
    # unrelated, so their edge pixels are extended instead
    if wrap is None:
        return 'edge'
    return 'symmetric' if wrap >= 2 else 'wrap'


def pad_image(arr, pad_y, pad_x, wrap):
    arr = np.pad(arr, ((pad_y, pad_y), (0, 0)) + ((0, 0),) * (arr.ndim - 2), mode=pad_mode(wrap[1]))
    return np.pad(arr, ((0, 0), (pad_x, pad_x)) + ((0, 0),) * (arr.ndim - 2), mode=pad_mode(wrap[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--upscaler', required=True, help='path to realesrgan-ncnn-vulkan(.exe)')
    ap.add_argument('--in', dest='src', default='build/run/Textures/eca_dump')
    ap.add_argument('--out', dest='dst', default='build/run/Textures/eca')
    ap.add_argument('--model', default='realesrgan-x4plus', choices=sorted(MODEL_SCALE))
    ap.add_argument('--scale', type=int, default=4, choices=(2, 3, 4),
                    help='final size relative to the original (the model always runs at 4x)')
    ap.add_argument('--detail', type=float, default=1.0,
                    help='back-projection strength: 1 restores the original grain, 0 keeps the pure Real-ESRGAN output')
    ap.add_argument('--gpu', default='auto', help='Vulkan GPU id for Real-ESRGAN (default: auto)')
    ap.add_argument('--force', action='store_true', help='redo textures that already have an HD version')
    args = ap.parse_args()

    if not os.path.isfile(args.upscaler):
        sys.exit(f'upscaler not found: {args.upscaler}')
    model_dir = os.path.join(os.path.dirname(os.path.abspath(args.upscaler)), 'models')
    os.makedirs(args.dst, exist_ok=True)
    wraps = read_wrap_modes(args.src)
    names = sorted(f[:-4] for f in os.listdir(args.src) if f.endswith('.png'))
    todo = [n for n in names if args.force or not os.path.exists(os.path.join(args.dst, n + '.png'))]
    print(f'{len(names)} dumped textures, {len(todo)} to upscale with {args.model} at {args.scale}x')
    if not todo:
        return

    model_scale = MODEL_SCALE[args.model]
    # Work next to the output: the intermediate images run to gigabytes, too much for a
    # RAM-backed /tmp
    work = tempfile.mkdtemp(prefix='.upscale_work_', dir=os.path.dirname(os.path.abspath(args.dst)) or '.')
    try:
        src_dir, out_dir = os.path.join(work, 'in'), os.path.join(work, 'out')
        os.makedirs(src_dir)
        os.makedirs(out_dir)
        meta = {}
        for n in todo:
            rgba = np.array(Image.open(os.path.join(args.src, n + '.png')).convert('RGBA'))
            h, w = rgba.shape[:2]
            wrap = (None, None) if n.startswith('blk_') else wraps.get(n, (0, 0))
            pad_y, pad_x = min(16, h), min(16, w)
            rgb = pad_image(bleed_transparent(rgba), pad_y, pad_x, wrap)
            Image.fromarray(rgb, 'RGB').save(os.path.join(src_dir, n + '.png'))
            meta[n] = (rgba, w, h, pad_x, pad_y, wrap)

        cmd = [args.upscaler, '-i', src_dir, '-o', out_dir, '-n', args.model, '-s', str(model_scale),
               '-f', 'png', '-m', model_dir]
        if args.gpu != 'auto':
            cmd += ['-g', args.gpu]
        print('running Real-ESRGAN...', flush=True)
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if r.returncode != 0:
            sys.exit('Real-ESRGAN failed:\n' + r.stderr[-2000:])

        done = 0
        for n, (rgba, w, h, pad_x, pad_y, wrap) in meta.items():
            up_path = os.path.join(out_dir, n + '.png')
            if not os.path.exists(up_path):
                print(f'  missing output for {n}', file=sys.stderr)
                continue
            up = np.array(Image.open(up_path).convert('RGB'))
            s = model_scale
            up = up[pad_y * s:(pad_y + h) * s, pad_x * s:(pad_x + w) * s]
            out_w, out_h = w * args.scale, h * args.scale
            rgb = Image.fromarray(up, 'RGB')
            if args.scale != model_scale:
                rgb = rgb.resize((out_w, out_h), Image.LANCZOS)
            rgb = Image.fromarray(back_project(np.asarray(rgb), bleed_transparent(rgba), args.detail), 'RGB')

            alpha = rgba[..., 3]
            if (alpha == 255).all():
                a = Image.new('L', (out_w, out_h), 255)
            else:
                # Resize alpha with the same wrap-aware padding so edges match the colour
                pa = pad_image(alpha, pad_y, pad_x, wrap)
                big = Image.fromarray(pa, 'L').resize((pa.shape[1] * args.scale, pa.shape[0] * args.scale), Image.LANCZOS)
                a = np.array(big)[pad_y * args.scale:(pad_y + h) * args.scale, pad_x * args.scale:(pad_x + w) * args.scale]
                if set(np.unique(alpha)) <= {0, 255}:
                    a = np.where(a >= 128, 255, 0).astype(np.uint8)   # 1-bit alpha stays 1-bit
                a = Image.fromarray(a, 'L')
            out = rgb.convert('RGBA')
            out.putalpha(a)
            out.save(os.path.join(args.dst, n + '.png'), optimize=False)
            done += 1
        print(f'wrote {done} HD textures to {args.dst}')
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
