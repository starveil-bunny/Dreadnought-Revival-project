#!/usr/bin/env bash
# Companion to havoc-camsnap.txt: ends wave 1 early (/wave skip) 10 s into it,
# so the wave 2 announcement lands while the steps file takes screenshots,
# then /kill all 50 s later to exercise the respawn path.
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
off=$(stat -c %s "$L")
for _ in $(seq 600); do
  if tail -c +"$off" "$L" | grep -q 'wave 1 state 2 t+10'; then
    dn cmd "/wave skip"; date +%T
    # then a death: respawn path for the ship-switch logging
    sleep 50; dn cmd "/kill all"; date +%T
    exit 0
  fi
  sleep 1
done
echo "timed out waiting for wave 1" >&2; exit 1
