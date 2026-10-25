#!/usr/bin/env bash
# Companion to ffa-colors.txt (2026-10-24): does a /boss get a marker on the
# client? /boss runs BossMarker at spawn (Havoc: PVE_HordeManager_BP_C.
# AddBossMarker + MulticastShowHUDMarkerOnActor); /host bosshere then parks
# the boss in view, re-runs the marker, then the spotted path (0x592BB0).
# Frames into $DN_CYCLE_DIR/boss/, then the host's "boss marker:" lines.
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
S=${DN_CYCLE_DIR:-/tmp}/boss; mkdir -p "$S"
off=$(stat -c %s "$L")
since() { tail -c +"$off" "$L"; }
grab() { ffmpeg -loglevel error -y -f x11grab -video_size 1280x720 -i "${DN_CLIENT_DISPLAY:-:97}" -frames:v 1 "$S/$1.png"; }
for _ in $(seq 400); do since | grep -aq 'ship physics: movement component .* remote player' && break; sleep 1; done
sleep 95
grab before
echo "== $(date +%T) /boss -> $(dn cmd "/boss ${DN_BOSS_ID:-}" 2>&1 | tail -1)"
sleep 5; grab b05
echo "== $(dn cmd "/host bosshere ${DIST:-3500} 0 ${LIFT:-0}" 2>&1 | tail -1)"    # parked in view, spawn-time marker only
for i in 1 2 3; do sleep 2; grab "c-here-$i"; done
echo "== $(dn cmd "/host bosshere ${DIST:-3500} -1 ${LIFT:-0}" 2>&1 | tail -1)" # parked again, BossMarker re-run
for i in 1 2 3; do sleep 2; grab "d-remark-$i"; done
echo "== $(dn cmd "/host bosshere ${DIST:-3500} 100 ${LIFT:-0}" 2>&1 | tail -1)" # spotted path
for i in 1 2 3; do sleep 2; grab "e-spot-$i"; done
since | grep -a 'boss marker\|boss:' | grep -v 'boss marker: fn \|enum EYUI' | cut -c1-220 | tail -30
