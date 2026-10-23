#!/usr/bin/env bash
# Companion to havoc-kill.txt: /kill all 10 s into wave 1. A kill during the
# orbit intro (pawn already spawned) did nothing visible, headless 10-16 20:48.
# Then /nuke 40 s later (after the respawn) to check enemies die too.
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
off=$(stat -c %s "$L")
for _ in $(seq 600); do
  if tail -c +"$off" "$L" | grep -q 'wave 1 state 2 t+10'; then
    dn cmd "/kill all"; date +%T
    [ "${NUKE:-1}" = 0 ] && exit 0
    sleep 40; dn cmd "/nuke"; date +%T; exit 0
  fi
  sleep 1
done
echo "timed out waiting for spawn" >&2; exit 1
