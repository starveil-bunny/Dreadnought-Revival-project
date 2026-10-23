#!/usr/bin/env bash
# Companion to ons-boss-meteor.txt: /boss, /swarm, /squad, /rain, /droplets, /wave
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
off=$(stat -c %s "$L")
# the battle host starts a fresh log when the match launches (nuke3 waited
# forever past the end of the old one): a new inode, or a file shorter than
# the start offset, is read from the top. (since runs in pipeline subshells,
# so it cannot just reset off.)
ino=$(stat -c %i "$L")
since() {
  local o=$off
  if [ "$(stat -c %i "$L")" != "$ino" ] || [ "$(stat -c %s "$L")" -lt "$off" ]; then o=1; fi
  tail -c +"$o" "$L"
}
for _ in $(seq 900); do since | grep -aq 'ship physics: movement component .* remote player' && break; sleep 1; done
since | grep -aq 'ship physics: movement component .* remote player' || { echo "timed out waiting for spawn" >&2; exit 1; }
sleep 95   # orbit intro + fly-out
c() { echo "== $(date +%T) $1"; dn cmd "$1"; }
c "/boss"; sleep 25
c "/swarm 24"; c "/squad 8"; sleep 25
c "/droplets 60"; sleep 75
c "/wave"; sleep 40
echo "== $(date +%T) log"
since | grep -a 'commands:\|boss\|herald\|rain\|swarm\|squad\|droplet\|marker\|team 3\|died' | grep -av 'creep VH_ONS_AITargetL' | tail -120
