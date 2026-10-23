#!/usr/bin/env bash
# Companion to sky-meteor.txt: /meteor 12 ten seconds into wave 1.
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
off=$(stat -c %s "$L")
for _ in $(seq 600); do
  if tail -c +"$off" "$L" | grep -q 'wave 1 state 2 t+10'; then
    dn cmd "/meteor 12"; date +%T
    sleep 30; tail -c +"$off" "$L" | grep -a "meteor" | tail -6; exit 0
  fi
  sleep 1
done
echo "timed out waiting for spawn" >&2; exit 1
