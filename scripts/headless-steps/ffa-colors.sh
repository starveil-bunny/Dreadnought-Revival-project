#!/usr/bin/env bash
# Companion to ffa-colors.txt (2026-10-24): what colour / relation does the
# client give a ship of each team? Per team: /dummy T (an uncontrolled ship
# 4000 uu ahead of the player), a frame of the nameplate, a frame while the
# right button is held (missile lock: module slots blue -> green), and a frame
# while firing (damage numbers: red = enemy, green = "healing" = friendly).
# Pass 1 with the player on its stock team, pass 2 after /host ffa on (the
# player's ship on team 3). Frames in $DN_CYCLE_DIR/sweep/, then a colour tally.
# TEAMS / FFA_TEAMS override the lists.
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
X="python3 $(dirname "$0")/../xdrive.py ${DN_CLIENT_DISPLAY:-:97}"
S=${DN_CYCLE_DIR:-/tmp}/sweep; mkdir -p "$S"
off=$(stat -c %s "$L")
since() { tail -c +"$off" "$L"; }
grab() { ffmpeg -loglevel error -y -f x11grab -video_size 1280x720 -i "${DN_CLIENT_DISPLAY:-:97}" -frames:v 1 "$S/$1.png"; }
for _ in $(seq 400); do since | grep -aq 'ship physics: movement component .* remote player' && break; sleep 1; done
since | grep -aq 'ship physics: movement component .* remote player' || { echo "timed out waiting for spawn" >&2; exit 1; }
sleep 12 # orbit drop finished, control
sweep() { # $1 pass label, rest teams
  local p=$1; shift
  for t in "$@"; do
    echo "== $(date +%T) $p team $t -> $(dn cmd "/dummy $t ${SHIP:-AssaultHeavyT4} ${DIST:-1200}" 2>&1 | tail -1)"
    sleep 4
    since | grep -a 'dummy:' | tail -1 | cut -c1-200
    # frame a: nameplate colour + module slots (a lockable enemy in range
    # turns the Drain Missile slot green). The right button is the
    # artillery zoom, not a lock, so no held-button frame.
    grab "$p-t$t-a"
    ( sleep 1.5; grab "$p-t$t-dmg" ) & $X button 1 3; wait
  done
}
sweep base ${TEAMS:-0 1 2 3 4 5 6 7 8 9 10 50 255}
echo "== $(date +%T) /host ffa on -> $(dn cmd "/host ffa on 50" 2>&1 | tail -1)"
sleep 8
since | grep -a 'ffa: controller' | tail -2 | cut -c1-200
sweep ffa ${FFA_TEAMS:-1 2 3 4 5}
dn cmd "/dummy off" >/dev/null 2>&1
python3 - "$S" <<'PY'
import sys, os, colorsys
from PIL import Image
d = sys.argv[1]
def tally(path):
    im = Image.open(path).convert("RGB").crop((340, 160, 940, 560)).resize((150, 100))
    c = {"red": 0, "green": 0, "blue": 0, "purple": 0, "yellow": 0}
    for r, g, b in im.getdata():
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        if s < .45 or v < .45: continue
        h *= 360
        k = "red" if h < 20 or h >= 340 else "yellow" if h < 70 else "green" if h < 160 else "blue" if h < 250 else "purple"
        c[k] += 1
    return " ".join(f"{k}{v:>5}" for k, v in c.items())
for f in sorted(os.listdir(d)):
    print(f"{f:22s} {tally(os.path.join(d, f))}")
PY
