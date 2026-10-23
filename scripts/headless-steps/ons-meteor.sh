#!/usr/bin/env bash
# Companion to ons-boss-meteor.txt without the boss: /meteor 12 once the
# player has control, then the meteor log lines (herald weapons, probe).
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
# 90 s of video from the command (a 2 km drop takes longer than 40 s): a nuke falls for 1-3 s, the 15 s
# screenshots miss it (2026-10-17 nuke2)
ffmpeg -loglevel error -y -f x11grab -video_size 1280x720 -framerate 10 -i "${DN_CLIENT_DISPLAY:-:97}" -t 90 \
  "${DN_CYCLE_DIR:-/tmp}/meteor.mp4" &
echo "== $(date +%T) /meteor 12 ${METEOR_UP:-}"; dn cmd "/meteor 12 ${METEOR_UP:-}"
wait
sleep 20
echo "== $(date +%T) log"
since | grep -a 'meteor\|herald\|kill\|died' | tail -80
