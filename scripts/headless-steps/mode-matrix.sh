#!/usr/bin/env bash
# Companion to mode-matrix.txt: once the player has control, every fun chat
# command in turn (MATRIX_CMDS, ;-separated, overrides the list), MATRIX_GAP s apart, then each command's host log lines (from its
# "commands: #N" line to the next one) so a sweep shows what each mode did.
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
echo "== mode: $(since | grep -aoE 'building the match table for mode "[^"]+"' | tail -1)"
sleep "${MATRIX_LEAD:-10}"   # was 95; no command needs the match to age
cmds=("/boss 1" "/boss" "/spawn AssaultHeavyT4" "/spawn AssaultHeavyT4 1 x3" "/turbo on" "/turbo off" "/npc hard"
      "/bots 2" "/shake 5" "/flash red" "/slowmo 0.5" "/slowmo 1" "/meteor" "/nuke" "/difficulty legendary"
      "/wave skip" "/kill all")
[ -n "$MATRIX_CMDS" ] && IFS=';' read -ra cmds <<<"$MATRIX_CMDS"
for c in "${cmds[@]}"; do
  echo "== $(date +%T) $c -> $(dn cmd "$c" 2>&1 | tail -1)"
  sleep "${MATRIX_GAP:-8}"
done
sleep 5   # last command's log lines
echo "== host log per command"
since | grep -aE 'commands:|appearance:|elite|boss marker|meteor|herald|turbo|slowmo|nuke|bots x|onslaught creeps|fault|EXCEPTION' \
  | grep -avE 'spawn abilities -- .*re-applied|tracked shot|percept search|meteor -- probe' | cut -c1-260 | tail -400
