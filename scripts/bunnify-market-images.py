#!/usr/bin/env python3
"""Bunny ears (and a tail) on every market picture.

Reads data/market-images/*.png (the hero icons gen-hero-market-data.py decodes
from the client's cooked textures) and writes the same file names to
data/market-images-bunny/. mmogbrain serves that directory instead when
DN_BUNNY_IMAGES=1. Cosmetic only: no id, name or size changes, so the catalog
is identical either way.

The ship is shrunk to 78% to make room; ears stand on the hull's top edge 40%
along from the stern, the tail on the stern at its vertical middle. Drawn at 4x and
downsampled for smooth edges.
"""
import glob
import math
import os
import sys

from PIL import Image, ImageDraw

SS = 4
WHITE = (250, 246, 250, 255)
PINK = (246, 168, 196, 255)
OUTLINE = (60, 50, 60, 255)


def ear(draw, base, length, width, angle_deg):
    """One ear: an ellipse from base outwards at angle (0 = straight up)."""
    a = math.radians(angle_deg)
    cx = base[0] + math.sin(a) * length / 2
    cy = base[1] - math.cos(a) * length / 2

    def ellipse(l, w, colour, outline=None):
        pts = []
        for i in range(48):
            t = 2 * math.pi * i / 48
            x, y = w / 2 * math.cos(t), l / 2 * math.sin(t)
            pts.append((cx + x * math.cos(a) - y * math.sin(a), cy + x * math.sin(a) + y * math.cos(a)))
        draw.polygon(pts, fill=colour, outline=outline)
        return pts

    ellipse(length + 2 * SS, width + 2 * SS, OUTLINE)
    ellipse(length, width, WHITE)
    ellipse(length * 0.72, width * 0.45, PINK)


def bunnify(src, dst):
    orig = Image.open(src).convert("RGBA")
    w, h = orig.size
    # Shrink the ship to 78% and drop it low so the ears and tail fit the
    # unchanged canvas.
    k = 0.78
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    small = orig.resize((int(w * k), int(h * k)), Image.LANCZOS)
    im.alpha_composite(small, (int(w * (1 - k) * 0.65), h - small.size[1]))
    alpha = im.getchannel("A")
    box = alpha.point(lambda v: 255 if v > 128 else 0).getbbox()
    if not box:
        orig.save(dst)
        return
    px = alpha.load()
    hull_w, hull_h = box[2] - box[0], box[3] - box[1]

    def top_at(x):
        for y in range(box[1], box[3]):
            if px[x, y] > 128:
                return y
        return box[3]

    # Ears on the hull's top edge 40% along from the stern (left), where the
    # bridge usually is; the nose points right.
    tx = box[0] + int(hull_w * 0.40)
    base_y = min(top_at(x) for x in range(tx - 6, tx + 7)) + 6
    length = max(40, min(int(base_y * 0.95), int(h * 0.36)))
    width = length * 0.36
    layer = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for dx, ang in ((-0.5, -16), (0.5, 10)):
        ear(d, ((tx + dx * width) * SS, base_y * SS), length * SS, width * SS, ang)
    # Tail: a cotton ball half behind the stern, at its vertical middle.
    sx = box[0] + 3
    ly = [y for y in range(box[1], box[3]) if px[sx, y] > 128] or [(box[1] + box[3]) // 2]
    r = max(9, hull_h * 0.10)
    cx, cy = (sx + r * 0.2) * SS, (sum(ly) / len(ly)) * SS
    r *= SS
    d.ellipse((cx - r - 2 * SS, cy - r - 2 * SS, cx + r + 2 * SS, cy + r + 2 * SS), fill=OUTLINE)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=WHITE)
    layer = layer.resize((w, h), Image.LANCZOS)
    # Hull over ears and tail, so both look like they grow from it.
    Image.alpha_composite(layer, im).save(dst)


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
    src_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "market-images")
    dst_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "market-images-bunny")
    os.makedirs(dst_dir, exist_ok=True)
    n = 0
    for f in sorted(glob.glob(os.path.join(src_dir, "*.png"))):
        bunnify(f, os.path.join(dst_dir, os.path.basename(f)))
        n += 1
    print(f"{n} pictures -> {dst_dir}")


if __name__ == "__main__":
    main()
