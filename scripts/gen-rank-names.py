#!/usr/bin/env python3
"""Fill in DN_Ranks_Player's rank names -- the player's rank title -- into
data/datatables/Progression/Ranks/DN_Ranks_Player.json.

Why the file needs filling in. tools/bpdump writes every FText property as the
literal string "[text]": it does not decode FText at all, in this table or in
the 12 other dumped tables (430 occurrences). For most of those the text is in
the localization files and the key is what matters; here the text is the value,
it is stored inline in the asset (the table is not localized), and the client
shows it as the rank title on the player card -- m_rankName is named by
UI_Generic_CaptainRank/UI_Persistent_PlayerCard's RankTitle, and every name is
an FText literal in DN_Ranks_Player.uasset.

How the names are read. Each row of the cooked table is an FText stored as a
16-byte GUID (the FText's key) followed by the string itself; the row KEYS
("0".."50") are serialized separately, before the row data. The strings are
therefore taken in file order and paired with the keys in the file's own order
-- which is how the dump already lists them. The result is checked against the
ladder: the names must be exactly as many as the rows, and must not contain a
placeholder.

Idempotent: rewrites m_rankName in place, leaving the file's shape alone.

  gen-rank-names.py [game-tree]
"""
import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "data", "datatables", "Progression", "Ranks", "DN_Ranks_Player.json")
PAKX = os.path.join(REPO, "tools", "pakx.py")
ASSET = "DN_Ranks_Player.uasset"
PAKS = os.environ.get(
    "DN_PAKS",
    os.path.expanduser("~/.local/share/Steam/steamapps/common/Dreadnought/"
                       "DreadGame/DreadGame/Content/Paks"))

# The asset's own declarations, and the row keys, are ASCII runs too. A rank
# name is the run that follows one of the FText GUIDs.
GUID = re.compile(rb"[0-9A-F]{32}")
RUN = re.compile(rb"[\x20-\x7e]{2,}")


def rank_names(blob):
    names = []
    for m in GUID.finditer(blob):
        tail = blob[m.end():m.end() + 160]
        n = RUN.search(tail)
        if n and n.group().decode().strip():
            names.append(n.group().decode().strip())
    return names


def find_asset(game):
    if game is None:
        tmp = tempfile.mkdtemp(prefix="dn-ranks-")
        subprocess.run([sys.executable, PAKX, PAKS, re.escape(ASSET), tmp], check=True)
        root = os.path.join(tmp, "DreadGame", "Content")
    else:
        root = game
        for rel in ("DreadGame/Content", "Content", ""):
            if os.path.isdir(os.path.join(root, rel, "Generic", "DataTables")):
                root = os.path.join(root, rel) if rel else root
                break
    for dirpath, _, files in os.walk(root):
        if ASSET in files:
            return os.path.join(dirpath, ASSET)
    return None


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else None
    src = find_asset(game)
    if not src:
        sys.exit("MISSING: %s not found (is the game installed? DN_PAKS?)" % ASSET)
    with open(src, "rb") as f:
        names = rank_names(f.read())
    raw = open(OUT, "rb").read()
    doc = json.loads(raw)
    keys = list(doc["rows"])
    if len(names) != len(keys):
        sys.exit("MISMATCH: %d names in %s, %d rows in %s -- refusing to write"
                 % (len(names), ASSET, len(keys), OUT))
    for key, name in zip(keys, names):
        doc["rows"][key]["m_rankName"] = name
    if doc.get("row_count") not in (None, len(keys)):
        sys.exit("MISMATCH: row_count %r but %d rows" % (doc["row_count"], len(keys)))
    # Same shape bpdump writes (2-space indent), and a trailing newline only if
    # the file had one.
    text = json.dumps(doc, indent=2, ensure_ascii=False) + ("\n" if raw.endswith(b"\n") else "")
    with open(OUT, "w") as f:
        f.write(text)
    print("%d rank names -> %s" % (len(names), OUT))
    print("  rank 1: %s ... rank %d: %s" % (names[0], len(names), names[-1]))


if __name__ == "__main__":
    main()
