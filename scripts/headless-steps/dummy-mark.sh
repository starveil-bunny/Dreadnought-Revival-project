#!/usr/bin/env bash
# Companion (2026-10-24): the boss marker on a ship that is in view. Parks a
# /dummy in front of the player, then runs the host's BossMarker on it
# (/host dummymark: AddBossMarker + MulticastShowHUDMarkerOnActor on Havoc),
# then the spotted path (/host dummymark 100). Frames into $DN_CYCLE_DIR/mark/.
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
S=${DN_CYCLE_DIR:-/tmp}/mark; mkdir -p "$S"
off=$(stat -c %s "$L")
since() { tail -c +"$off" "$L"; }
grab() { ffmpeg -loglevel error -y -f x11grab -video_size 1280x720 -i "${DN_CLIENT_DISPLAY:-:97}" -frames:v 1 "$S/$1.png"; }
for _ in $(seq 400); do since | grep -aq 'ship physics: movement component .* remote player' && break; sleep 1; done
sleep 95
echo "== $(dn cmd "/dummy ${TEAM:-2} ${SHIP:-AssaultHeavyT4} ${DIST:-2500}" 2>&1 | tail -1)"
sleep 4; grab a-plain
echo "== $(dn cmd "/host dummymark" 2>&1 | tail -1)"
for i in 1 2 3 4; do sleep 3; grab "b-default-$i"; done
echo "== $(dn cmd "/host dummymark 100" 2>&1 | tail -1)"
for i in 1 2 3; do sleep 3; grab "c-spotted-$i"; done
since | grep -a 'boss marker\|dummy:' | grep -v 'boss marker: fn ' | cut -c1-220 | tail -30
