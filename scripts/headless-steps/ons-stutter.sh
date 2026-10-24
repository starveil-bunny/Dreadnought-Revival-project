#!/usr/bin/env bash
# Companion to ons-short.txt: /wave then /meteor, then the host's stutter and
# timing lines (preload, spawn class loads, elite steps, ready time).
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
off=$(stat -c %s "$L")
ino=$(stat -c %i "$L")
since() {
  local o=$off
  if [ "$(stat -c %i "$L")" != "$ino" ] || [ "$(stat -c %s "$L")" -lt "$off" ]; then o=1; fi
  tail -c +"$o" "$L"
}
for _ in $(seq 900); do since | grep -aq 'ship physics: movement component .* remote player' && break; sleep 1; done
since | grep -aq 'ship physics: movement component .* remote player' || { echo "timed out waiting for spawn" >&2; exit 1; }
echo "== $(date +%T) spawned"
sleep ${DN_STUTTER_WAIT:-45}
c() { echo "== $(date +%T) $1"; dn cmd "$1"; }
c "/wave"; sleep 30
c "/kill boss"; sleep 5
c "/meteor"; sleep 30
c "/clone me"; sleep 15
echo "== $(date +%T) log"
since | grep -a 'appearance:\|Evil \|boss .* killed\|net trace\|salvo\|herald .* done\|boss marker\|marker\|ready time:\|preload:\|meteor timing\|stutter\|tick: \|elite timing\|spawn [A-Za-z]*T5 -> /Game.*ms)\|commands: #' | cut -c1-260 | tail -120
