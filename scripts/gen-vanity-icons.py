#!/usr/bin/env python3
"""Cosmetic pictures for the market: decode each cosmetic's own icon out of the
client paks into data/market-images/, and write data/vanity/VanityIcons.json
(item id -> file name) for the server to serve.

Why this exists. The market's only per-item picture path is the catalog entity's
image fields (full_image_url / ImageURL / ImgUrlS/M/L, gateway_catalog.go), and
nothing filled them for cosmetics: 1,183 vanity entities went out with every one
of those empty, so the tile fell back to whatever the client draws for an
unknown picture. The client's own cooked data states the icon per item --
data/vanity/VanityItems_cooked.jsonl m_itemUIData.m_icon / m_highResIcon -- and
the art is in the paks, so the picture can be the client's own rather than
invented.

What is NOT an icon, and must be skipped rather than substituted:
  * 218 items whose m_icon names the item's own material under
    /Game/Generic/VanityItems/ -- not a Texture2D, undecodable;
  * 19 items with no m_icon at all.
Both get no entry, so the server sends no picture for them, which is the state
they were already in.

The 687 items whose m_icon names a SHARED per-slot glyph under
/Game/Generic/UI/vanity/ are not a bug in the data and are decoded like any
other: that glyph IS the client's own choice for those items. UI_Vanity_Coating,
which 76 coating items name (385810556 "Isolation" among them), decodes to the
white water-drop-in-a-ring the operator sees on those tiles.

  gen-vanity-icons.py [game-tree]

game-tree is an already-extracted Content tree; when omitted the needed assets
are extracted from the shipped paks with tools/pakx.py into a temp dir (the
paks are the only copy on a player install).
"""
import atexit
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import tex2png  # noqa: E402  (decode(): cooked Texture2D bytes -> PIL image)

VANITY = os.path.join(REPO, "data", "vanity", "VanityItems_cooked.jsonl")
OUT_JSON = os.path.join(REPO, "data", "vanity", "VanityIcons.json")
OUT_IMG = os.path.join(REPO, "data", "market-images")
PAKX = os.path.join(REPO, "tools", "pakx.py")
PAKS = os.environ.get(
    "DN_PAKS",
    os.path.expanduser("~/.local/share/Steam/steamapps/common/Dreadnought/"
                       "DreadGame/DreadGame/Content/Paks"))


def object_name(path):
    """'/Game/A/B.UI_C.UI_C' -> ('UI_C', 'A/B/UI_C.uasset')"""
    package = path.split(".")[0]
    return os.path.basename(package), package[len("/Game/"):] + ".uasset"


def decode(path):
    """The texture at path, or None: tex2png finds no PF_* format in a
    non-texture (materials, meshes), which is how those are told apart."""
    try:
        with open(path, "rb") as f:
            return tex2png.decode(f.read())
    except Exception:
        return None


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else None
    rows = [json.loads(l) for l in open(VANITY)]
    items, seen = [], set()  # (item id, icon path, high-res path)
    for row in rows:
        item_id = (row.get("m_itemSystemData") or {}).get("m_itemID")
        if not item_id:
            continue
        if item_id in seen:  # the jsonl carries more than one row per asset
            continue
        seen.add(item_id)
        ui = row.get("m_itemUIData") or {}
        items.append((item_id, (ui.get("m_icon") or "").strip(),
                      (ui.get("m_highResIcon") or "").strip()))

    wanted = {}
    for _, icon, hi in items:
        for p in (icon, hi):
            if p:
                wanted[object_name(p)[1]] = object_name(p)[0]

    if game is None:
        # pakx writes the mount-relative path, which starts at "DreadGame/".
        tmp = tempfile.mkdtemp(prefix="dn-pak-")
        atexit.register(shutil.rmtree, tmp, True)
        # One pass: alternation of the object names we need. Per-icon calls
        # would rescan every pak (~60) once per asset.
        # Name plus its extension: the entry names are "UI_X.uasset", so a dot
        # after the name never occurs and a path-shaped pattern matches nothing.
        pattern = "|".join(re.escape(n + ".uasset") for n in sorted(set(wanted.values())))
        subprocess.run([sys.executable, PAKX, PAKS, pattern, tmp], check=True)
        content = os.path.join(tmp, "DreadGame", "Content")
    else:
        content = os.path.join(game, "Content") if game else content
    os.makedirs(OUT_IMG, exist_ok=True)
    manifest, written, cache = {}, {}, {}
    skipped_noicon = skipped_notexture = 0
    shrunk = []
    not_texture, high_res_saved = [], []
    reported = {}
    def decodable(path):
        """The asset's file name once it is decoded and written, else None."""
        if not path:
            return None
        name, rel = object_name(path)
        if name not in cache:
            image = decode(os.path.join(content, rel))
            cache[name] = image is not None
            if image is not None:
                # The client's own icon textures are 512 on the long edge; three
                # items name a 1024/2048 full-res texture as their icon, which
                # would cost ~4 MB per tile for pixels no tile shows.
                if max(image.size) > 512:
                    shrunk.append(name)
                    image = image.convert("RGBA")
                    image.thumbnail((512, 512), tex2png.Image.LANCZOS)
                image.convert("RGBA").save(os.path.join(OUT_IMG, name + ".png"))
                written[name] = rel
        return name if cache[name] else None

    for item_id, icon, hi in items:
        if not icon and not hi:
            skipped_noicon += 1
            continue
        chosen = decodable(icon)
        if chosen is None and icon:
            not_texture.append(item_id)
        if chosen is None and hi and hi != icon:
            chosen = decodable(hi)
            if chosen is not None:
                high_res_saved.append(item_id)
        if chosen:
            manifest[str(item_id)] = chosen + ".png"
        else:
            skipped_notexture += 1
        reported[item_id] = (icon, hi, chosen)

    with open(OUT_JSON, "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    print("%d cosmetic rows: %d icons written, %d items left with no picture (%d have no icon at all, %d have nothing decodable)"
          % (len(items), len(written), skipped_noicon + skipped_notexture, skipped_noicon, skipped_notexture))
    print("m_icon is NOT a decodable Texture2D for %d items%s" % (
        len(not_texture), "" if not not_texture else " (e.g. %s)" % not_texture[:8]))
    print("fell back to m_highResIcon for %d items" % len(high_res_saved))
    print("%d icons downscaled to 512 (textures larger than any tile): %s" % (len(shrunk), shrunk[:4]))
    for item_id in sorted(reported):
        if item_id in (385810556, 385810573, 419365113):
            icon, hi, chosen = reported[item_id]
            print("  %d: m_icon=%s -> %s" % (item_id, icon or "<none>", chosen + ".png" if chosen else "<no picture>"))
    print(len(manifest), "items ->", OUT_JSON)


if __name__ == "__main__":
    main()
