#!/usr/bin/env python3
"""Real bunny photos as the market pictures.

For every data/market-images/*.png, writes a bunny photo of the same name and
size (512x320, centre-cropped to fill) to data/market-images-bunny/, which
mmogbrain serves instead when DN_BUNNY_IMAGES=1. Photos come from the bunny
repo (~/Projects/bunny-repo/index.jsonl, images only, or $BUNNY_REPO); each
market picture gets a different photo, chosen by a seeded shuffle so re-runs
give the same pictures (--seed N for another set). Replaces the drawn
ears-and-tail versions of scripts/bunnify-market-images.py.
"""
import glob
import json
import os
import random
import sys

from PIL import Image, ImageOps

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "data", "market-images")
DST = os.path.join(ROOT, "data", "market-images-bunny")
REPO = os.environ.get("BUNNY_REPO", os.path.expanduser("~/Projects/bunny-repo"))


def main():
    seed = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 1
    rows = [json.loads(l) for l in open(os.path.join(REPO, "index.jsonl")) if l.strip()]
    photos = [os.path.join(REPO, r["file"]) for r in rows
              if r.get("mime", "").startswith("image/") and (r.get("w") or 0) >= 512 and (r.get("h") or 0) >= 320
              and os.path.exists(os.path.join(REPO, r["file"]))]
    photos.sort()
    random.Random(seed).shuffle(photos)
    names = sorted(glob.glob(os.path.join(SRC, "*.png")))
    if len(photos) < len(names):
        sys.exit(f"only {len(photos)} usable photos for {len(names)} pictures")
    os.makedirs(DST, exist_ok=True)
    for name, photo in zip(names, photos):
        size = Image.open(name).size
        img = ImageOps.fit(ImageOps.exif_transpose(Image.open(photo)).convert("RGB"), size, Image.LANCZOS, centering=(0.5, 0.3))
        img.save(os.path.join(DST, os.path.basename(name)))
    print(f"{len(names)} pictures -> {DST}")


if __name__ == "__main__":
    main()
