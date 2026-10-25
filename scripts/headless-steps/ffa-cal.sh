#!/usr/bin/env bash
# FFA colour calibration helpers for a `dn live` client (2026-10-24).
#   ffa-cal.sh enter           victory/home -> Onslaught -> first ship -> flying
#   ffa-cal.sh spawn T [uu] [ship]   dummy in front of the nose, frames at 0/1/3 s
#   ffa-cal.sh sweep [T...]    per team: frame at 3 s -> ffa-np.py (bar/name colour, lock)
#   ffa-cal.sh damage T        missiles (key 1) at a team-T dummy, frame at 2.5 s
# Calibrated on cycle 20261024-190133-ffa-ons: 5000 uu = "999.9m" on the HUD
# (1 m = 5 uu; 250 m sits inside the player's hull, 2500 uu drew no nameplate);
# the dummy needs ~3 s (0 s still shows the last one, 1 s the plate slides in),
# then holds still (DummyHoldTick); a damage number lasts ~2.5-4 s. Never
# click/move during a sweep: the pointer is mouse-look and pitches the nose.
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
case $1 in
enter)
  dn live click 90 69 >/dev/null; sleep 8          # BACK TO HOME (no-op on home)
  dn live click 645 446 >/dev/null; sleep 2        # popup
  dn live click 107 298 >/dev/null; sleep 3        # PLAY
  dn live click 353 380 >/dev/null; sleep 3        # Recruit
  dn live click 206 260 >/dev/null; sleep 2        # Onslaught
  dn live click 640 158 >/dev/null                 # PLAY
  timeout 300 dn live wait --new ClientLoadingCompleted 240 | tail -1; sleep 8
  dn live click 380 620 >/dev/null; dn live key space >/dev/null
  timeout 120 dn live wait --new CallOnPlayerSpawned | tail -1; sleep 25
  dn live shot flying | head -1 ;;
spawn)
  t=$2; uu=${3:-25000}; ship=${4:-AssaultHeavyT4}
  dn cmd "/dummy $t $ship $uu" | tail -1
  for s in 0 1 3; do sleep $((s == 0 ? 0 : s == 1 ? 1 : 2)); dn live shot "t$t-${uu}-${s}s" | head -1; done
  grep -a 'dummy:' "$L" | tail -1 | cut -c1-260 ;;
sweep)
  shift; teams=${*:-0 1 2 3 4 5 6 7 8 9 10}; S=$(ls -d run/cycles/*-ffa-ons | tail -1)/shots
  for t in $teams; do
    dn cmd "/dummy $t AssaultHeavyT4 ${UU:-5000}" >/dev/null; sleep 3
    f=$(dn live shot "sweep-t$t" | sed -n 's/^shot: //p' | head -1)
    echo "team $t: $(python3 scripts/headless-steps/ffa-np.py "$f" | cut -d' ' -f2-)"
  done ;;
damage)
  dn cmd "/dummy $2 AssaultHeavyT4 ${UU:-5000}" >/dev/null; sleep 3
  dn live key 1 >/dev/null; sleep 2.5
  f=$(dn live shot "dmg-t$2" | sed -n 's/^shot: //p' | head -1)
  python3 scripts/headless-steps/ffa-np.py "$f" ;;
esac
