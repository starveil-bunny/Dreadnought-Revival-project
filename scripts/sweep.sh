#!/bin/bash
# sweep.sh LIST [--secs N] [--steps FILE] -- pinned headless matches back to back, one line each.
#
# LIST: one pin per line, `LABEL PIN` (PIN = MODE[:MAP] or JSON, see
# `dn headless --pin`); # comments. Every match replays pg-battle.txt (hangar
# -> PLAY -> pick ship -> fly); the pin swaps the mode/map server-side, so the
# client flow is the same for every mode. Results: run/cycles/sweep-<ts>.txt
# (verdict, milestones reached, battle-host crash), full bundles per match;
# each run's dn headless output in run/cycles/sweep-<ts>.log (a driver that
# died mid-run left nothing behind, sweep 2026-10-12 22:46 havoc-map2).
# Waits for :443 + health first, so it can be queued before the stack is up.
#
# Every run is judged on reaching the battle: the pin is what the sweep is
# testing, so a match that never loads a map is a failure, not a result. That
# was not always caught -- on 2026-10-17 the Space02 cycle froze on the tier
# screen, never queued, and its line read "NO-EXPECTATIONS ... no host crash",
# indistinguishable from a pass. It now carries the battle-map milestone as
# --expect (the same regex cycle.py uses, client log) and prints NO-BATTLE when
# that milestone is missing, whatever the verdict says.
set -u
cd "$(dirname "$0")/.."
list=$1; shift; secs=420; steps=scripts/headless-steps/pg-battle.txt
# --steps: PvE/Havoc waves wait for ReadyUp after spawn (havoc-ready.txt)
while [[ $# -ge 2 ]]; do
  case $1 in --secs) secs=$2 ;; --steps) steps=$2 ;; *) echo "sweep: unknown option $1" >&2; exit 2 ;; esac
  shift 2
done
# "the match reached a map", not "the client started": cycle.py's battle-map
# milestone (MILESTONES, scripts/cycle.py).
battle_re='LoadMap\(/Game/Maps/(MP|Havoc|PVE)/'
out=run/cycles/sweep-$(date +%Y%m%d-%H%M%S).txt
until ss -ltn | grep -q ':443 ' && [[ $(curl -s -m2 -o /dev/null -w '%{http_code}' http://127.0.0.1:8083/health) == 200 ]]; do sleep 10; done
# a host left over from an earlier sweep (or a chained one still in its own
# end wait) takes the first login back into its match: 2026-10-13 havoc-map2
# joined the previous sweep's PodTDM host. Same wait as between runs.
wait_no_host() {
  for _ in $(seq 36); do
    [[ $(curl -s -m2 http://127.0.0.1:8085/instances | grep -o '"count":[0-9]*') == '"count":0' ]] && return
    sleep 5
  done
}
wait_no_host
echo "# sweep $list $(date +%T)" > "$out"
grep -vE '^\s*(#|$)' "$list" | while read -r label pin; do
  t0=$(date +%Y%m%d-%H%M%S)
  ./scripts/dn headless "sw-$label" --steps "$steps" --pin "$pin" --secs "$secs" \
    --expect "$battle_re" >> "${out%.txt}.log" 2>&1 < /dev/null
  # only a bundle this run made: an older one with the same label would hand
  # its verdict to a run that never started (2026-10-13, refused launch got
  # yesterday's havoc-map2 result)
  d=$(ls -d run/cycles/*-sw-"$label" 2>/dev/null | sort | tail -1)
  if [[ -z $d || ${d#run/cycles/} < $t0 || ! -f $d/summary.md ]]; then
    printf '%-14s %-60s %s\n' "$label" "$pin" "NO BUNDLE (see ${out%.txt}.log)" >> "$out"
  else
  v=$(grep -m1 '^VERDICT' "$d/summary.md"); m=$(grep -m1 '^milestones' "$d/summary.md" | grep -oE '[A-Za-z-]+@' | tr -d @ | tr '\n' ' ')
  # a battle-*.log line under "crash markers" = the host died, not the client
  hc=$(grep -qE '^- battle-[^:]*: .*(Fatal error|ACCESS_VIOLATION|Critical error)' "$d/summary.md" && echo HOST-CRASH)
  # any marker from another log is the client's (or the mod's): before this the
  # column read "no host crash", which was true and useless -- on 2026-10-17 the
  # Space02 client stopped dead and the sweep line said nothing had crashed.
  if [[ -z $hc ]]; then
    sec=$(sed -n '/^## crash markers/,/^$/p' "$d/summary.md")
    if [[ -n $sec ]]; then
      if [[ $(grep -c '^- battle-' <<<"$sec") -lt $(grep -c '^- ' <<<"$sec") ]]; then
        hc="CLIENT-CRASH"
      else
        hc="CRASH-MARKERS"   # in a battle log but not the fatal pattern: check the summary
      fi
    fi
  fi
  # No battle-map milestone = the run never got into a match, so nothing about
  # the pin was tested. Say that instead of leaving a bare FAIL (or, before the
  # --expect above, NO-EXPECTATIONS) on the line. A run with crash markers keeps
  # its verdict: a client that died on the way is a different failure, and the
  # summary's crash section names it.
  if [[ " $m " != *" battle-map "* ]] && ! grep -q '^## crash markers' "$d/summary.md"; then
    v="VERDICT: NO-BATTLE"
  fi
  printf '%-14s %-60s %s | %s | %s\n' "$label" "$pin" "${v#VERDICT: }" "$m" "${hc:-no host crash}" >> "$out"
  fi
  # A killed client is only dropped at the engine's ~60 s net timeout (+10 s
  # reap); log in before that and mmogbrain sends the client back into the
  # old match with no UI or pawn -- every run after the first was that
  # (sweep 2026-10-12 21:42). Wait until no host is left, up to 3 min.
  wait_no_host
done
echo "# done $(date +%T)" >> "$out"
