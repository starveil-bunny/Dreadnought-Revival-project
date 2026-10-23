#!/usr/bin/env bash
# Companion to mode-matrix.txt: does a /spawn'd AI ship fly and fight?
# Once the player has control: /spawn (SPAWN_CMD), then FIGHT_SECS of viewer
# snapshots (2 s apart) for the ships that appeared after it -- distance moved,
# HP, nearest enemy -- plus HP drops of every other ship. No screenshots.
L=$(dirname "$DREADGAME_EXE")/dn_host_loadout.log
off=$(stat -c %s "$L")
since() { tail -c +"$off" "$L"; }
for _ in $(seq 300); do since | grep -aq 'ship physics: movement component .* remote player' && break; sleep 1; done
since | grep -aq 'ship physics: movement component .* remote player' || { echo "timed out waiting for spawn" >&2; exit 1; }
echo "== mode: $(since | grep -aoE 'building the match table for mode "[^"]+"' | tail -1)"
sleep "${SPAWN_DELAY:-5}" # SPAWN_DELAY: e.g. 70 to /spawn after BeginBattle
KEY=$(grep -h '^ADMIN_KEY=' "$(dirname "$0")/../../run/secrets.env" | head -1 | cut -d= -f2- | tr -d '"')
api() { curl -sS -H "X-Admin-Key: $KEY" "http://127.0.0.1:8083/admin/api$1"; }
bid=$(api /battles | python3 -c 'import json,sys; b=json.load(sys.stdin)["battles"]; print(b[0]["battle_match_id"] if b else "")')
[ -n "$bid" ] || { echo "no battle in the viewer" >&2; exit 1; }
d=$(mktemp -d)
api "/battles/$bid" >"$d/0.json"
echo "== $(date +%T) ${SPAWN_CMD:=/spawn AssaultHeavyT4} -> $(dn cmd "$SPAWN_CMD" 2>&1 | tail -1)"
# SPAWN_CMD2: a second command in the same match (e.g. "/rain 40"), 10 s later
[ -n "$SPAWN_CMD2" ] && sleep 10 && echo "== $(date +%T) $SPAWN_CMD2 -> $(dn cmd "$SPAWN_CMD2" 2>&1 | tail -1)"
for i in $(seq 1 $(( ${FIGHT_SECS:-60} / 2 ))); do sleep 2; api "/battles/$bid" >"$d/$i.json"; done
python3 - "$d" <<'EOF'
import json, math, os, sys
d = sys.argv[1]
snaps = [json.load(open(f"{d}/{i}.json")) for i in sorted(int(f[:-5]) for f in os.listdir(d))]
key = lambda s: s.get("vid") or (s["class"], s["team"])
before = {key(s) for s in snaps[0].get("ships", [])}
track, hp = {}, {}
for t, sn in enumerate(snaps):
    ships = sn.get("ships", [])
    for s in ships:
        k = key(s)
        hp.setdefault(k, [s["name"], s["team"], s["npc"], []])[3].append(s["hp"])
        if k in before: continue
        foes = [math.dist((s["x"], s["y"], s["z"]), (o["x"], o["y"], o["z"])) for o in ships if o["team"] != s["team"]]
        track.setdefault(k, []).append((t * 2, s["x"], s["y"], s["z"], s["hp"], min(foes) if foes else -1))
print("== spawned ships (t s, pos, hp%, nearest enemy)")
for k, rows in track.items():
    moved = sum(math.dist(a[1:4], b[1:4]) for a, b in zip(rows, rows[1:]))
    print(f"-- {hp[k][0]} team {hp[k][1]}: moved {moved:.0f} uu over {rows[-1][0]-rows[0][0]} s, hp {rows[0][4]} -> {rows[-1][4]}")
    for r in rows[::5] + [rows[-1]]:
        print(f"   t+{r[0]:3d} {r[1]:9.0f} {r[2]:9.0f} {r[3]:9.0f} hp {r[4]:3d} foe {r[5]:7.0f}")
if not track: print("   none appeared")
print("== hp drops, other ships")
for k, (n, team, npc, h) in hp.items():
    if k not in track and h and min(h) < h[0]: print(f"   {n} team {team}{' npc' if npc else ''}: {h[0]} -> {min(h)}")
print("== last snapshot ships")
for s in snaps[-1].get("ships", []):
    print(f"   {s['name']} team {s['team']}{' npc' if s['npc'] else ''} {s.get('hull') or s['class']} hp {s['hp']}")
print("== projectiles per snapshot:", " ".join(str(len(s.get("projectiles") or [])) for s in snaps))
EOF
rm -rf "$d"
