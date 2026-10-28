#!/usr/bin/env python3
"""Inspect the natural/reference frame pairs that tests/test_win32.c saves on failure.

`build/test_win32.exe --artifacts DIR` writes DIR/QuickPhrase-check-<pid>-<tick>/
NNN-natural.bmp and NNN-reference.bmp for every capture whose two frames differ.
NNN is the 1-based capture index, so it matches the Nth "natural/reference:" line in
the test log. This tool answers the only question that matters when triaging such a
failure: is the difference the app repainting itself badly, or is it the screen
capture that does not hold our window at all (a tooltip, another application, an
in-flight restore animation)?

    python tools/win32_frame_diff.py stats  DIR [NNN ...]
    python tools/win32_frame_diff.py map    DIR NNN          # coarse diff density
    python tools/win32_frame_diff.py crop   DIR NNN          # zoomed side-by-side
    python tools/win32_frame_diff.py shot   DIR NNN ...      # natural|reference|diff

Needs numpy and Pillow (use the system interpreter if the managed one lacks them).
"""

import glob
import os
import struct
import sys

import numpy as np


def load_bmp(path):
    with open(path, 'rb') as handle:
        data = handle.read()
    if data[:2] != b'BM':
        raise ValueError('not a BMP: %s' % path)
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bits = struct.unpack_from('<H', data, 28)[0]
    top_down = height < 0
    height = abs(height)
    stride = ((width * bits // 8) + 3) // 4 * 4
    pixels = np.frombuffer(data, dtype=np.uint8, count=height * stride, offset=offset)
    pixels = pixels.reshape(height, stride)[:, :width * (bits // 8)]
    pixels = pixels.reshape(height, width, bits // 8)[:, :, :3]
    if not top_down:
        pixels = pixels[::-1]
    return pixels


def pair(directory, index):
    natural = os.path.join(directory, '%03d-natural.bmp' % index)
    reference = os.path.join(directory, '%03d-reference.bmp' % index)
    return natural, reference


def pairs(directory):
    found = {}
    for path in glob.glob(os.path.join(directory, '*-natural.bmp')):
        base = path[:-len('-natural.bmp')]
        if os.path.exists(base + '-reference.bmp'):
            found[os.path.basename(base)] = (path, base + '-reference.bmp')
    return found


def cmd_stats(directory, indices):
    items = [(os.path.basename(n), r) for n, r in
             (pair(directory, int(i)) for i in indices)] if indices else sorted(pairs(directory).items())
    for name, (natural, reference) in items:
        a = load_bmp(natural)
        b = load_bmp(reference)
        if a.shape != b.shape:
            print('%-10s SHAPE MISMATCH %s vs %s' % (name, a.shape, b.shape))
            continue
        diff = np.any(a != b, axis=2)
        count = int(diff.sum())
        if not count:
            print('%-10s %dx%d diff=0' % (name, a.shape[1], a.shape[0]))
            continue
        ys, xs = np.nonzero(diff)
        delta = np.abs(a.astype(int) - b.astype(int))
        print('%-10s %dx%d diff=%-8d %5.2f%%  bbox x[%d,%d] y[%d,%d]  meanAbs=%.2f'
              % (name, a.shape[1], a.shape[0], count, 100.0 * count / diff.size,
                 xs.min(), xs.max(), ys.min(), ys.max(), delta.mean()))


def cmd_map(directory, index):
    a = load_bmp(pair(directory, index)[0])
    b = load_bmp(pair(directory, index)[1])
    diff = np.any(a != b, axis=2)
    rows, cols = 26, 72
    height, width = diff.shape
    for row in range(rows):
        y0, y1 = row * height // rows, (row + 1) * height // rows
        line = ''
        for col in range(cols):
            x0, x1 = col * width // cols, (col + 1) * width // cols
            share = diff[y0:y1, x0:x1].mean()
            line += ' ' if share < 0.02 else ('.' if share < 0.15 else ('o' if share < 0.5 else '#'))
        print(line)
    print('diff=%d of %d pixels' % (int(diff.sum()), diff.size))


def _zoom(pixels, x0, y0, x1, y1, pad, factor):
    from PIL import Image
    height, width = pixels.shape[:2]
    x0, y0 = max(0, x0 - pad), max(0, y0 - pad)
    x1, y1 = min(width, x1 + pad), min(height, y1 + pad)
    crop = pixels[y0:y1, x0:x1]
    image = Image.fromarray(crop)
    return image.resize((crop.shape[1] * factor, crop.shape[0] * factor), Image.NEAREST)


def _side_by_side(left, right, gap=12, background=(30, 30, 30)):
    from PIL import Image
    height = max(left.height, right.height)
    canvas = Image.new('RGB', (left.width + right.width + gap, height), background)
    canvas.paste(left, (0, 0))
    canvas.paste(right, (left.width + gap, 0))
    return canvas


def cmd_crop(directory, index, out_dir, factor=8):
    a = load_bmp(pair(directory, index)[0])
    b = load_bmp(pair(directory, index)[1])
    diff = np.any(a != b, axis=2)
    ys, xs = np.nonzero(diff)
    if not len(ys):
        print('%03d has no difference' % index)
        return
    x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
    image = _side_by_side(_zoom(a, x0, y0, x1, y1, 14, factor),
                          _zoom(b, x0, y0, x1, y1, 14, factor))
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, '%03d-zoom.png' % index)
    image.save(path)
    print('%s  region x[%d,%d] y[%d,%d] diff=%d' % (path, x0, x1, y0, y1, int(diff.sum())))


def cmd_shot(directory, indices, out_dir):
    from PIL import Image
    os.makedirs(out_dir, exist_ok=True)
    for index in indices:
        a = load_bmp(pair(directory, index)[0])
        b = load_bmp(pair(directory, index)[1])
        diff = np.any(a != b, axis=2)
        highlighted = a.copy()
        highlighted[diff] = [255, 0, 0]
        height, width = a.shape[:2]
        scale = min(1.0, 900.0 / width, 700.0 / height)
        size = (max(1, int(width * scale)), max(1, int(height * scale)))
        panels = [Image.fromarray(x).resize(size, Image.NEAREST) for x in (a, b, highlighted)]
        path = os.path.join(out_dir, '%03d-cmp.png' % index)
        _side_by_side(panels[0], panels[1], 8, (20, 20, 20))
        canvas = Image.new('RGB', (size[0] * 3 + 16, size[1]), (20, 20, 20))
        for slot, panel in enumerate(panels):
            canvas.paste(panel, (slot * (size[0] + 8), 0))
        canvas.save(path)
        print('%s  %dx%d diff=%d' % (path, width, height, int(diff.sum())))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    command, directory = argv[1], argv[2]
    rest = argv[3:]
    if command == 'stats':
        cmd_stats(directory, rest)
    elif command == 'map':
        cmd_map(directory, int(rest[0]))
    elif command == 'crop':
        cmd_crop(directory, int(rest[0]), rest[1] if len(rest) > 1 else 'build/bmpview')
    elif command == 'shot':
        cmd_shot(directory, [int(x) for x in rest], 'build/bmpview')
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
