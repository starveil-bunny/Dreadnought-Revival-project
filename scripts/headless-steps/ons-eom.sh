#!/usr/bin/env bash
# Companion to ons-short.txt: /endmatch 1 a while after spawning, then the
# host's end-of-match personal stats lines.
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
sleep ${DN_EOM_WAIT:-60}
# DN_EOM_PROBE="cmd1;cmd2": host commands before /endmatch (e.g. "props pri PveRewards").
IFS=';' read -ra pre <<<"${DN_EOM_PROBE:-}"
for c in "${pre[@]}"; do dn cmd "/host $c" >/dev/null; sleep 2; done
echo "== $(date +%T) /endmatch 1"; dn cmd "/endmatch 1"
sleep 40
since | grep -a 'endmatch\|eom stats\|props\|EXCEPTION' | cut -c1-260 | tail -${DN_EOM_TAIL:-40}
