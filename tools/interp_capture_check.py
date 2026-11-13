#!/usr/bin/env python3
"""Finds flicker in an F4 capture of inserted images (test builds, docs/fps_altos.md).

F4 saves, for each frame, the first inserted image and the real one after it:
interp_<n>_<frame>_a_inserted.bmp / _b_real.bmp. An inserted image lies between two real frames, so
while things only move each of its pixels stays roughly between those two. A pixel that comes out
clearly darker or brighter than BOTH real neighbours flickers. Fast motion leaves a background of such
pixels (edges, reflections), so an image is flagged when its darker or brighter share stands out from the
capture's own median by --factor and exceeds --minimum percent.

  python3 interp_capture_check.py <capture dir> [<capture dir> ...] [--factor 2.5] [--minimum 1] [--margin 40]

Needs Pillow and numpy. Reads only the capture directories.
"""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image


def luminance(path):
    rgb = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def check(directory, factor, minimum, margin):
    inserted = sorted(glob.glob(os.path.join(directory, "*_a_inserted.bmp")))
    real = sorted(glob.glob(os.path.join(directory, "*_b_real.bmp")))
    if len(inserted) != len(real) or len(inserted) < 2:
        print(f"{directory}: not an F4 capture (inserted={len(inserted)} real={len(real)})")
        return 0
    shares = []
    before = luminance(real[0])
    for index in range(1, len(inserted)):
        after = luminance(real[index])
        middle = luminance(inserted[index])
        darker = middle < np.minimum(before, after) - margin
        brighter = middle > np.maximum(before, after) + margin
        shares.append((index, float(darker.mean()) * 100.0, float(brighter.mean()) * 100.0))
        before = after
    dark_median = float(np.median([dark for _, dark, _ in shares]))
    bright_median = float(np.median([bright for _, _, bright in shares]))
    flagged = [(index, dark, bright) for index, dark, bright in shares
               if (dark > minimum and dark > factor * dark_median) or
               (bright > minimum and bright > factor * bright_median)]
    print(f"{os.path.basename(os.path.normpath(directory))}: {len(shares)} inserted images, median "
          f"{dark_median:.2f}% darker / {bright_median:.2f}% brighter than both real neighbours, "
          f"{len(flagged)} flagged")
    for index, dark, bright in flagged:
        print(f"  image {index:3d}: {dark:.2f}% darker, {bright:.2f}% brighter than both real neighbours")
    return len(flagged)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directories", nargs="+")
    parser.add_argument("--factor", type=float, default=2.5, help="times the capture's median that counts")
    parser.add_argument("--minimum", type=float, default=1.0, help="never flag below this %% of pixels")
    parser.add_argument("--margin", type=float, default=40.0, help="luminance difference (0-255) that counts")
    args = parser.parse_args()
    total = sum(check(directory, args.factor, args.minimum, args.margin) for directory in args.directories)
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
