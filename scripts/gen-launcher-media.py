#!/usr/bin/env python3
"""Launcher media out of the client's own paks, into dn-launcher/media/.

Two things the launcher page dresses itself in that come from the game.

FONT. The launcher's text is the game's text, so it is set in a font the game
ships: Quantico, the TTF pair under DreadGame/Content/Slate/Fonts/ that the
developers added to the engine's own set (the rest are Roboto/DroidSans, which
are the engine's, not Dreadnought's), and which they also cooked into a UI font
of their own (/Game/Generic/Fonts/Quantico). The files are copied verbatim --
no rendering, nothing invented.

  NOT Rajdhani, which is the face the client's player card actually names in
  its FSlateFontInfo (/Game/UserInterface/Fonts/Rajdhani/Rajdhani-SemiBold,
  UMG's UFontFace). Those assets carry no font: UFontFace keeps its bytes in
  FFontFaceData, and in this cook that array is empty for all four Rajdhani
  weights -- no glyf/cmap/loca anywhere in the 141KB uasset, and no .ubulk
  beside it in the pak. There is no Rajdhani TTF in the paks at all (checked
  every .ttf entry), so it cannot become a webfont. Quantico is the closest
  thing that is really there, and it is a Dreadnought face.

ICONS. The account card's two resources, from
/Game/Generic/UI/sRGB_icons/currencies/: the sRGB pair, not the
/Game/Generic/UI/currencies/ or /Game/Generic/UI/ ones beside them, which are
the linear copies the material system wants and too dark for a web page.

  gen-launcher-media.py [game-tree]

game-tree is an already-extracted Content tree; omitted, the needed files are
extracted from the shipped paks with tools/pakx.py into a temp dir (the paks
are the only copy on a player install).
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import tex2png  # noqa: E402  (decode(): cooked Texture2D bytes -> PIL image)

OUT = os.path.join(REPO, "dn-launcher", "media")
PAKX = os.path.join(REPO, "tools", "pakx.py")
PAKS = os.environ.get(
    "DN_PAKS",
    os.path.expanduser("~/.local/share/Steam/steamapps/common/Dreadnought/"
                       "DreadGame/DreadGame/Content/Paks"))

# The client's own TTF files, copied byte for byte. Path -> name in media/.
FONTS = {
    "Slate/Fonts/Quantico-Regular.ttf": "Quantico-Regular.ttf",
    "Slate/Fonts/Quantico-Bold.ttf": "Quantico-Bold.ttf",
}

# The wallet's three resources. SoftCurrency is the credit balance (mmogbrain's
# soft_currency), HardCurrency the premium one (premium_currency, "gold" on the
# card) and FreeXP the free XP balance (player_state.free_xp). The client also
# ships a ShipXP glyph, but the account card's XP bar is rank progress and the
# client pairs no icon with it, so none is used rather than one guessed.
ICONS = {
    "UI_Currency_SoftCurrency_Large": "credit.png",
    "UI_Currency_HardCurrency_Large": "gold.png",
    "UI_Currency_FreeXP_Large": "freexp.png",
}

# Icon edge in the launcher. The client's *_Large pair is 512px for a tile this
# page draws at ~18px; 128 keeps it crisp on a HiDPI screen for a few KB.
ICON_EDGE = 128


def decode(path):
    try:
        with open(path, "rb") as f:
            return tex2png.decode(f.read())
    except Exception:
        return None


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else None
    names = [os.path.basename(p) for p in FONTS] + [n + ".uasset" for n in ICONS]
    if game is None:
        tmp = tempfile.mkdtemp(prefix="dn-launcher-media-")
        pattern = "|".join(re.escape(n) for n in names)
        subprocess.run([sys.executable, PAKX, PAKS, pattern, tmp], check=True)
        content = os.path.join(tmp, "DreadGame", "Content")
    else:
        # Accept the install root, the DreadGame folder, or the Content tree
        # itself: pakx writes DreadGame/Content/..., but an operator's own
        # extracted tree may start anywhere above it.
        content = game
        for rel in ("DreadGame/Content", "Content", ""):
            if os.path.isdir(os.path.join(content, rel, "Slate", "Fonts")):
                content = os.path.join(content, rel) if rel else content
                break

    def find(name, where):
        hits = glob.glob(os.path.join(content, where, "**", name), recursive=True)
        return hits[0] if hits else None

    os.makedirs(OUT, exist_ok=True)
    for rel, out in FONTS.items():
        src = find(os.path.basename(rel), os.path.dirname(rel).replace("/", os.sep))
        if not src:
            print("MISSING: %s" % rel)
            continue
        with open(src, "rb") as f:
            data = f.read()
        # A TTF starts with an sfnt version and a table directory; a wrong file
        # here (an icon, a uasset) would otherwise be shipped as a "font" and
        # silently fall back in the browser.
        if data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true") or data.find(b"glyf") < 0:
            print("MISSING: %s is not a TTF with outlines (%s)" % (rel, src))
            continue
        with open(os.path.join(OUT, out), "wb") as f:
            f.write(data)
        print("%-38s %7d bytes -> media/%s" % (rel, len(data), out))

    for name, out in ICONS.items():
        src = find(name + ".uasset", os.path.join("Generic", "UI", "sRGB_icons", "currencies"))
        if not src:
            print("MISSING: %s" % name)
            continue
        img = decode(src)
        if img is None:
            print("MISSING: %s does not decode as a texture (%s)" % (name, src))
            continue
        was = img.size
        img = img.convert("RGBA")
        if max(img.size) > ICON_EDGE:
            img.thumbnail((ICON_EDGE, ICON_EDGE), tex2png.Image.LANCZOS)
        img.save(os.path.join(OUT, out))
        print("%-38s %sx%s -> %sx%s media/%s" % (name, was[0], was[1], img.size[0], img.size[1], out))


if __name__ == "__main__":
    main()
