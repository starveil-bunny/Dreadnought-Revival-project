#!/usr/bin/env bash
# Companion: FFA per-player score + end rule in one headless TDM run (2026-10-20).
# Once the player has control: /host ffa on LIMIT (default 2), /spawn 3 game
# bots, then /nuke every NUKE_GAP s (kill credit goes to the human) until the
# host logs the FFA end or NUKES run out. Prints the ffa:/match-end log lines.
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
off=$(stat -c %s "$L")
since() { tail -c +"$off" "$L"; }
for _ in $(seq 300); do since | grep -aq 'ship physics: movement component .* remote player' && break; sleep 1; done
since | grep -aq 'ship physics: movement component .* remote player' || { echo "timed out waiting for spawn" >&2; exit 1; }
sleep 5
echo "== $(date +%T) /host ffa on ${LIMIT:=2} -> $(dn cmd "/host ffa on $LIMIT" 2>&1 | tail -1)"
sleep 8 # FfaTick sets the human's ship team on the next player tick
echo "== $(date +%T) /spawn AssaultHeavyT4 3 -> $(dn cmd "/spawn AssaultHeavyT4 3" 2>&1 | tail -1)"
# Before BeginBattle the bots are queued for the pre-match fill (gamebot-boss14):
# wait for them to be placed, at most 150 s.
for _ in $(seq 150); do since | grep -aq "queued /spawn(s) placed after the fill\|-> game bot, list entry" && break; sleep 1; done
echo "== $(date +%T) $(since | grep -ao 'bots: setup (0x3678F0): [^[]*\|-> game bot, list entry [^[]*' | tail -1)"
sleep 15
for i in $(seq "${NUKES:-4}"); do
  echo "== $(date +%T) /nuke -> $(dn cmd /nuke 2>&1 | tail -1)"
  sleep "${NUKE_GAP:-15}"
  since | grep -aq 'ffa: .*ending the match' && break
  since | grep -a 'nuke hit' | tail -1 | grep -q 'hit 0 ' && dn cmd "/spawn AssaultHeavyT4 2" >/dev/null 2>&1
done
sleep 10
echo "== host log"
since | grep -aE 'ffa:|nuke hit|EndMatch|winner|[Mm]an [Oo]f [Tt]he [Dd]ay|match result' | cut -c1-220 | tail -30
