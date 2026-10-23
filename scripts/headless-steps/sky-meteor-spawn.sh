#!/usr/bin/env bash
# Companion to sky-meteor.txt that does not need a wave: /meteor 12 25 s after
# the host spawns a player ship (Havoc's wave line never came on HVC_Map2,
# skymeteor4 17:55).
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
off=$(stat -c %s "$L")
for _ in $(seq 600); do
  if tail -c +"$off" "$L" | grep -aq 'SpawnDefaultPawnAtTransform(ctrl'; then
    sleep 25; dn cmd "/meteor 12"; date +%T
    sleep 40; tail -c +"$off" "$L" | grep -a "meteor" | tail -30; exit 0
  fi
  sleep 1
done
echo "timed out waiting for spawn" >&2; exit 1
