#!/usr/bin/env bash
# Companion to ons-boss-meteor.txt: /boss once the player has control, wait
# up to 150 s for a player death, then /meteor 12 and collect the log.
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
deaths() { since | grep -ac 'player.*killed\|killed.*player\|PlayerKilled\|died'; }
for _ in $(seq 900); do since | grep -aq 'ship physics: movement component .* remote player' && break; sleep 1; done
since | grep -aq 'ship physics: movement component .* remote player' || { echo "timed out waiting for spawn" >&2; exit 1; }
sleep 95   # orbit intro + fly-out
echo "== $(date +%T) /boss"; dn cmd "/boss ${DN_BOSS_ID:-}"
d0=$(deaths)
for _ in $(seq 150); do [ "$(deaths)" -gt "$d0" ] && break; sleep 1; done
echo "== $(date +%T) deaths $d0 -> $(deaths)"
echo "== $(date +%T) /meteor 12"; dn cmd "/meteor 12"
sleep 45
echo "== $(date +%T) log"
since | grep -a 'boss\|elite\|meteor\|herald\|kill\|died\|onslaught: [^s]' | grep -av 'creep VH_ONS_AITargetL' | tail -60
