package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/protocol"
	"github.com/gorilla/mux"
)

// Battle viewer for the admin dashboard. Each battle host posts its live ships
// every 2 s (battle-server-mod ViewerSnapshot): POST /battle/snapshot?match=
// <battle_match_id>, one "x y z team hp% npc pid class" line per ship, world
// units as K2_GetActorLocation returns them. Kept in memory only; a host that
// stops posting drops out after viewerStaleAfter. The dashboard draws them in
// a wireframe 3D view and sends commands to that one match through the same
// chat-command path as the server shell (handleChatCommand), targeted by the
// match's own chat room.

type viewerShip struct {
	X     float64 `json:"x"`
	Y     float64 `json:"y"`
	Z     float64 `json:"z"`
	Team  int     `json:"team"`
	HP    int     `json:"hp"` // percent, -1 unknown
	NPC   bool    `json:"npc"`
	PID   string  `json:"pid,omitempty"`
	Name  string  `json:"name"`
	Class string  `json:"class"`
	Kind  string  `json:"kind,omitempty"` // icon key: Corvette, Destroyer, ...
	// Hull: the class label ("Medium Destroyer T1") when Name is the bot's
	// own in-game name instead.
	Hull  string `json:"hull,omitempty"`
	label string
	// cloaked: the host's 12th field; kept off the wire, used to hide the ship
	// from enemy players in the launcher viewer.
	cloaked bool
	Player  bool `json:"player"`
	// Yaw is the actor's heading in degrees (host K2_GetActorRotation), nil
	// from a host without it.
	Yaw *float64 `json:"yaw,omitempty"`
	// VID is the host's per-pawn viewer id (stable for the pawn's life), 0
	// from a host without it.
	VID int `json:"vid,omitempty"`
	// Me: the launcher viewer's own ship.
	Me bool `json:"me,omitempty"`
}

type viewerSnapshot struct {
	at    time.Time
	ships []viewerShip
	proj  [][3]float64
	kinds string
	ids   []int32
}

const viewerStaleAfter = 15 * time.Second

var viewerState struct {
	mu    sync.Mutex
	snaps map[string]viewerSnapshot // battle_match_id -> latest
}

func battleSnapshotHandler(w http.ResponseWriter, r *http.Request) {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		http.Error(w, "loopback only", http.StatusForbidden)
		return
	}
	match := r.URL.Query().Get("match")
	if match == "" || len(match) > 96 {
		http.Error(w, "match required", http.StatusBadRequest)
		return
	}
	body, _ := io.ReadAll(io.LimitReader(r.Body, 64<<10))
	ships, proj, kinds, ids := parseViewerSnapshot(bytes.NewReader(body))
	storeViewerScores(match, body) // launcher_viewer.go
	viewerState.mu.Lock()
	if viewerState.snaps == nil {
		viewerState.snaps = map[string]viewerSnapshot{}
	}
	viewerState.snaps[match] = viewerSnapshot{at: time.Now(), ships: ships, proj: proj, kinds: kinds, ids: ids}
	viewerState.mu.Unlock()
	w.WriteHeader(http.StatusNoContent)
}

// parseViewerSnapshot reads the host's snapshot: ship lines
// "x y z team hp% npc pid class [yaw [vid]]" and projectile lines
// "p x y z [kind [id]]" (kind m missile, b ballistic, p other; "p" when
// absent; id the host's GObjects index, -1 when absent).
func parseViewerSnapshot(body io.Reader) ([]viewerShip, [][3]float64, string, []int32) {
	ships := []viewerShip{}
	var proj [][3]float64
	var kinds []byte
	var ids []int32
	sc := bufio.NewScanner(body)
	for sc.Scan() && len(ships) < 512 {
		f := strings.Fields(sc.Text())
		if len(f) >= 4 && len(f) <= 6 && f[0] == "p" {
			var p [3]float64
			ok := len(proj) < 512
			for i := 0; i < 3 && ok; i++ {
				v, err := strconv.ParseFloat(f[1+i], 64)
				p[i], ok = v, err == nil
			}
			if ok {
				proj = append(proj, p)
				k := byte('p')
				if len(f) >= 5 && (f[4] == "m" || f[4] == "b") {
					k = f[4][0]
				}
				kinds = append(kinds, k)
				id := int32(-1)
				if len(f) == 6 {
					if v, err := strconv.ParseInt(f[5], 10, 32); err == nil {
						id = int32(v)
					}
				}
				ids = append(ids, id)
			}
			continue
		}
		if len(f) < 8 {
			continue
		}
		var s viewerShip
		var err error
		if s.X, err = strconv.ParseFloat(f[0], 64); err != nil {
			continue
		}
		if s.Y, err = strconv.ParseFloat(f[1], 64); err != nil {
			continue
		}
		if s.Z, err = strconv.ParseFloat(f[2], 64); err != nil {
			continue
		}
		s.Team, _ = strconv.Atoi(f[3])
		s.HP, _ = strconv.Atoi(f[4])
		s.NPC = f[5] == "1"
		if f[6] != "-" {
			s.PID = protocol.NormalizePlayerPID(f[6])
		}
		s.Class = f[7]
		if len(f) > 8 {
			if y, err := strconv.ParseFloat(f[8], 64); err == nil {
				s.Yaw = &y
			}
		}
		if len(f) > 9 {
			s.VID, _ = strconv.Atoi(f[9])
		}
		// The NPC's in-game name (its PlayerState's PlayerName), percent-encoded.
		if len(f) > 10 && f[10] != "-" {
			if n, err := url.PathUnescape(f[10]); err == nil {
				s.label = n
			}
		}
		s.cloaked = len(f) > 11 && f[11] == "1"
		ships = append(ships, s)
	}
	if len(ids) > 0 && ids[0] < 0 {
		ids = nil // an older host: no ids at all
	}
	return ships, proj, string(kinds), ids
}

// viewerHullKinds: pawn class stem -> ship class, as data/assets/HullNames.json
// pairs the precast loadouts (VH_Assault* Destroyer, VH_Scout* Corvette, ...).
// The values double as the dashboard's icon keys (UI_ClassIcons_<key>).
var viewerHullKinds = []struct{ stem, kind, label string }{
	{"Assault", "Destroyer", "Destroyer"},
	{"Dread", "Dreadnought", "Dreadnought"},
	{"Scout", "Corvette", "Corvette"},
	{"Sniper", "ArtilleryCruiser", "Artillery Cruiser"},
	{"Support", "TacticalCruiser", "Tactical Cruiser"},
}

var viewerOnsKinds = map[byte][2]string{
	'H': {"CommandShip", "Command Ship"},
	'M': {"AttackBoat", "Attack Boat"},
	'L': {"FighterSwarm", "Fighter Swarm"},
}

// viewerShipKind: ship class and a short label for a pawn class
// ("VH_SniperL_AI_Pawn_T2_BP_C" -> "ArtilleryCruiser", "Light Artillery Cruiser T2").
// Pawns outside the five hull families keep viewerShipName.
func viewerShipKind(class string) (kind, label string) {
	n := strings.TrimPrefix(strings.TrimPrefix(class, "HVC_"), "VH_")
	// Onslaught's AI targets. GUESS: H = command ship, M = attack boat,
	// L = fighter swarm (the user's three ONS roles; the size letter is the
	// only thing the class names give).
	if rest, ok := strings.CutPrefix(n, "ONS_AITarget"); ok && rest != "" {
		if k, ok := viewerOnsKinds[rest[0]]; ok {
			return k[0], k[1]
		}
	}
	for _, h := range viewerHullKinds {
		if !strings.HasPrefix(n, h.stem) {
			continue
		}
		rest := n[len(h.stem):] + "_"
		size := map[byte]string{'L': "Light ", 'M': "Medium ", 'H': "Heavy "}[rest[0]]
		label = size + h.label
		if i := strings.Index(rest, "_T"); i >= 0 && i+2 < len(rest) && rest[i+2] >= '1' && rest[i+2] <= '9' {
			label += " T" + rest[i+2:i+3]
		}
		if strings.Contains(rest, "Boss") {
			label += " (boss)"
		}
		return h.kind, label
	}
	return "", viewerShipName(class)
}

// viewerShipName: the class without its blueprint decoration
// ("VH_ONS_AITargetM_Pawn_BP_C" -> "ONS AITargetM").
func viewerShipName(class string) string {
	n := strings.TrimSuffix(strings.TrimSuffix(class, "_C"), "_BP")
	n = strings.TrimPrefix(strings.TrimPrefix(n, "HVC_"), "VH_")
	n = strings.Replace(n, "_Pawn", "", 1)
	return strings.ReplaceAll(n, "_", " ")
}

type viewerBattle struct {
	BattleMatchID string `json:"battle_match_id"`
	MatchID       string `json:"match_id,omitempty"`
	Mode          string `json:"mode,omitempty"`
	Map           string `json:"map,omitempty"`
	ModeName      string `json:"mode_name"`
	MapName       string `json:"map_name"`
	Terrain       bool   `json:"terrain"`
	// TerrainAt: when the terrain was uploaded (unix ms). The host posts a
	// second sweep ~30 s after the first (sublevels still streaming in during
	// the first), so the viewer refetches whenever this changes.
	TerrainAt int64 `json:"terrain_at,omitempty"`
	AgeMs     int64 `json:"age_ms"`
	// TakenMs: when mmogbrain received the snapshot (Unix ms). The viewer
	// stamps samples with it instead of reply arrival minus age_ms, which
	// moved a sample late by however long its reply took.
	TakenMs int64        `json:"taken_ms"`
	Players int          `json:"players"`
	Ships   []viewerShip `json:"ships,omitempty"`
	// Projectiles: live projectile positions from the same snapshot.
	Projectiles [][3]float64 `json:"projectiles,omitempty"`
	// ProjKinds: one letter per Projectiles entry, m missile, b ballistic,
	// p other.
	ProjKinds string `json:"proj_kinds,omitempty"`
	// ProjIDs: per Projectiles entry, the host's object index for the shot.
	ProjIDs []int32 `json:"proj_ids,omitempty"`
}

func viewerBattles(withShips bool) []viewerBattle {
	viewerState.mu.Lock()
	out := []viewerBattle{}
	for id, s := range viewerState.snaps {
		age := time.Since(s.at)
		if age > viewerStaleAfter {
			delete(viewerState.snaps, id)
			continue
		}
		b := viewerBattle{BattleMatchID: id, AgeMs: age.Milliseconds(), TakenMs: s.at.UnixMilli()}
		// Fallback count: non-NPC ships in the host's latest snapshot. Kept
		// because a battle the mmog database does not know about (a host
		// started without matchmaking through us) has no roster to read.
		for _, sh := range s.ships {
			if !sh.NPC {
				b.Players++
			}
		}
		if withShips {
			b.Ships = append([]viewerShip(nil), s.ships...)
			b.Projectiles = s.proj
			b.ProjKinds = s.kinds
			b.ProjIDs = s.ids
		}
		out = append(out, b)
	}
	viewerState.mu.Unlock()
	database := currentMmogPlayerStateDB()
	names := map[string]string{}
	for i := range out {
		b := &out[i]
		if m, ok := viewerDBCache.meta(b.BattleMatchID); ok {
			b.MatchID, b.Mode, b.Map = m.matchID, m.mode, m.mapPath
			if m.roster > 0 {
				b.Players = m.roster
			}
		} else if database != nil {
			_ = database.QueryRow(`SELECT id, game_mode, map FROM matches WHERE battle_match_id=? ORDER BY created_at DESC LIMIT 1`,
				b.BattleMatchID).Scan(&b.MatchID, &b.Mode, &b.Map)
			var roster int
			// Players is the match's roster, not the ships in the snapshot. The
			// host can only report a ship once a pawn exists, so a player who is
			// loaded in -- matched, in orbit, waiting on the spawn -- counted as
			// zero ("Conquest · Skybridge 0 players" while the operator was
			// flying it, 2026-10-17). match_slots is written when the player is
			// placed in the match and its row is deleted when the match ends or
			// the player leaves (response_builders.go), so any row is a live
			// roster. A roster of zero keeps the ship count rather than erasing
			// it: the two are then evidence of different things, and only the
			// roster can name a player the host has not spawned yet.
			if b.MatchID != "" {
				if database.QueryRow(`SELECT COUNT(DISTINCT user_id) FROM match_slots WHERE match_id=?`,
					b.MatchID).Scan(&roster) == nil && roster > 0 {
					b.Players = roster
				}
			}
			viewerDBCache.putMeta(b.BattleMatchID, viewerMeta{matchID: b.MatchID, mode: b.Mode, mapPath: b.Map, roster: roster})
		}
		b.ModeName, b.MapName = viewerModeName(b.Mode), viewerMapName(b.Map)
		if t := viewerTerrainFor(b.BattleMatchID); t != nil {
			b.Terrain, b.TerrainAt = true, t.at.UnixMilli()
		}
		for j := range b.Ships {
			s := &b.Ships[j]
			s.Player = !s.NPC
			s.Kind, s.Name = viewerShipKind(s.Class)
			if s.label != "" {
				s.Hull, s.Name = s.Name, s.label
			}
			if s.PID == "" || database == nil {
				continue
			}
			if n, ok := names[s.PID]; ok {
				s.Name = n
				continue
			}
			if n, ok := viewerDBCache.name(s.PID); ok {
				if n != "" {
					s.Name = n
				}
				names[s.PID] = s.Name
				continue
			}
			var n string
			if database.QueryRow(`SELECT COALESCE(display_name,'') FROM player_state WHERE user_id=?`, s.PID).Scan(&n) == nil && n != "" {
				s.Name = n
			}
			viewerDBCache.putName(s.PID, n)
			names[s.PID] = s.Name
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].BattleMatchID < out[j].BattleMatchID })
	return out
}

// viewerMapName: a PvE map's event name from DN_Events_DT (m_map -> m_name,
// "S1E4 - Escort ..."-style rows), else the MP map folder, with Space02 under
// its in-game name (GlobalUI "Ryugu Haven", see serverChatModeNames).
func viewerMapName(path string) string {
	if path == "" {
		return ""
	}
	events := loadPVETables().events
	// PvE matches store the event row name ("PVE_S1E4"), not the map path.
	if ev, ok := events[path]; ok && ev.Name.Text != "" {
		return ev.Name.Text
	}
	for _, ev := range events {
		if p, _, _ := strings.Cut(ev.Map, "."); p == path && ev.Name.Text != "" {
			return ev.Name.Text
		}
	}
	switch n := adminMapName(path); n {
	case "Space02":
		return "Ryugu Haven"
	default:
		return n
	}
}

func viewerModeName(mode string) string {
	if n, ok := serverChatModeNames[mode]; ok {
		return n
	}
	if strings.EqualFold(mode, "PVE") {
		return "PvE"
	}
	return mode
}

// --- terrain ----------------------------------------------------------------

// Each host samples its map's collision once per match (battle-server-mod
// TerrainTick) and posts it here: "grid nx ny x0 y0 dx dy", then ny rows of nx
// z values ("n" = nothing below). Kept for viewerTerrainKeep after the post.
type viewerTerrain struct {
	at time.Time
	NX int        `json:"nx"`
	NY int        `json:"ny"`
	X0 float64    `json:"x0"`
	Y0 float64    `json:"y0"`
	DX float64    `json:"dx"`
	DY float64    `json:"dy"`
	Z  []*float64 `json:"z"` // row-major, nil = miss; for grid2 the highest top
	// Cells (grid2 only), row-major: "n", or comma-separated surfaces of the
	// column, kind letter + z: t terrain, o object, w water (tops, entered
	// from above), b undersides, r/x the top of the warning/warp boundary
	// volume over that column (host mod TraceColumn).
	Cells []string `json:"cells,omitempty"`
	// Boxes are the map's boundary volumes (the host mod's BoundsProbe):
	// kind warp|warning|height|other, world-space min/max corners.
	Boxes []viewerBox `json:"boxes"`
	// Objects: one box per structure/rock/prop actor the fine pass hit (host
	// mod ObjectBoxes, "obj <hits> lo hi"), its colliding bounds; Kind "obj",
	// Hits the column tops that landed on it.
	Objects []viewerBox `json:"objects"`
}

type viewerBox struct {
	Kind string     `json:"kind"`
	Lo   [3]float64 `json:"lo"`
	Hi   [3]float64 `json:"hi"`
	Hits int        `json:"hits,omitempty"`
	// H (objects only): the actor's top-down shape over its xy bounds,
	// "nx ny z,z,..." row-major, "n" = the ray missed it (host ObjectBoxes).
	H string `json:"h,omitempty"`
	B string `json:"b,omitempty"` // underside, same cells as H
}

const viewerTerrainKeep = 3 * time.Hour

var viewerTerrains struct {
	mu sync.Mutex
	m  map[string]*viewerTerrain
}

func parseViewerTerrain(body io.Reader) *viewerTerrain {
	sc := bufio.NewScanner(body)
	sc.Buffer(make([]byte, 64<<10), 1<<20)
	if !sc.Scan() {
		return nil
	}
	f := strings.Fields(sc.Text())
	if len(f) != 7 || (f[0] != "grid" && f[0] != "grid2") {
		return nil
	}
	layered := f[0] == "grid2"
	t := &viewerTerrain{at: time.Now()}
	var err error
	if t.NX, err = strconv.Atoi(f[1]); err != nil || t.NX < 2 || t.NX > 256 {
		return nil
	}
	if t.NY, err = strconv.Atoi(f[2]); err != nil || t.NY < 2 || t.NY > 256 {
		return nil
	}
	for i, dst := range []*float64{&t.X0, &t.Y0, &t.DX, &t.DY} {
		if *dst, err = strconv.ParseFloat(f[3+i], 64); err != nil {
			return nil
		}
	}
	for len(t.Z) < t.NX*t.NY && sc.Scan() {
		for _, v := range strings.Fields(sc.Text()) {
			if layered {
				top, ok := parseViewerCell(v)
				if !ok {
					return nil
				}
				t.Cells = append(t.Cells, v)
				t.Z = append(t.Z, top)
			} else if z, err := strconv.ParseFloat(v, 64); err == nil {
				t.Z = append(t.Z, &z)
			} else {
				t.Z = append(t.Z, nil)
			}
		}
	}
	if len(t.Z) != t.NX*t.NY {
		return nil
	}
	t.Boxes, t.Objects = []viewerBox{}, []viewerBox{}
	for sc.Scan() {
		line := sc.Text()
		if strings.HasPrefix(line, "objh ") {
			if n := len(t.Objects); n > 0 && t.Objects[n-1].H == "" && len(line) < 8192 {
				t.Objects[n-1].H = line[5:]
			}
			continue
		}
		if strings.HasPrefix(line, "objb ") {
			if n := len(t.Objects); n > 0 && t.Objects[n-1].B == "" && len(line) < 8192 {
				t.Objects[n-1].B = line[5:]
			}
			continue
		}
		f := strings.Fields(line)
		if len(f) != 8 || (f[0] != "box" || len(t.Boxes) >= 64) && (f[0] != "obj" || len(t.Objects) >= 4096) {
			continue
		}
		b := viewerBox{Kind: f[1]}
		if f[0] == "obj" {
			n, err := strconv.Atoi(f[1])
			if err != nil {
				continue
			}
			b = viewerBox{Kind: "obj", Hits: n}
		}
		ok := true
		for i := 0; i < 6 && ok; i++ {
			v, err := strconv.ParseFloat(f[2+i], 64)
			ok = err == nil
			if i < 3 {
				b.Lo[i] = v
			} else {
				b.Hi[i-3] = v
			}
		}
		if ok && b.Kind == "obj" {
			t.Objects = append(t.Objects, b)
		} else if ok {
			t.Boxes = append(t.Boxes, b)
		}
	}
	return t
}

// parseViewerCell checks one grid2 cell and returns its highest top (nil if
// it has none).
func parseViewerCell(v string) (*float64, bool) {
	if v == "n" {
		return nil, true
	}
	var top *float64
	for _, s := range strings.Split(v, ",") {
		if len(s) < 2 || !strings.ContainsRune("towbrx", rune(s[0])) {
			return nil, false
		}
		z, err := strconv.ParseFloat(s[1:], 64)
		if err != nil {
			return nil, false
		}
		if strings.ContainsRune("tow", rune(s[0])) && (top == nil || z > *top) {
			top = &z
		}
	}
	return top, true
}

func battleTerrainHandler(w http.ResponseWriter, r *http.Request) {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		http.Error(w, "loopback only", http.StatusForbidden)
		return
	}
	match := r.URL.Query().Get("match")
	if match == "" || len(match) > 96 {
		http.Error(w, "match required", http.StatusBadRequest)
		return
	}
	t := parseViewerTerrain(io.LimitReader(r.Body, 16<<20)) // grid2 256x256 runs to a few MB
	if t == nil {
		http.Error(w, "bad grid", http.StatusBadRequest)
		return
	}
	viewerTerrains.mu.Lock()
	if viewerTerrains.m == nil {
		viewerTerrains.m = map[string]*viewerTerrain{}
	}
	for id, old := range viewerTerrains.m {
		if time.Since(old.at) > viewerTerrainKeep {
			delete(viewerTerrains.m, id)
		}
	}
	viewerTerrains.m[match] = t
	viewerTerrains.mu.Unlock()
	_, _ = w.Write([]byte("ok\n")) // 200: the host's poster treats anything else as a failure
}

func viewerTerrainFor(id string) *viewerTerrain {
	viewerTerrains.mu.Lock()
	defer viewerTerrains.mu.Unlock()
	return viewerTerrains.m[id]
}

func adminAPIBattleTerrain(w http.ResponseWriter, r *http.Request) {
	t := viewerTerrainFor(mux.Vars(r)["id"])
	if t == nil {
		http.Error(w, `{"error":"no terrain for that battle"}`, http.StatusNotFound)
		return
	}
	writeAdminJSON(w, t)
}

// viewerChatLine is one line of a match's chat rooms (All and Team).
type viewerChatLine struct {
	At   string `json:"at"`
	Team bool   `json:"team"`
	Name string `json:"name"`
	Text string `json:"text"`
}

func viewerMatchChat(matchID string) []viewerChatLine {
	if c, ok := viewerDBCache.chat(matchID); ok {
		return c
	}
	out := viewerMatchChatQuery(matchID)
	viewerDBCache.putChat(matchID, out)
	return out
}

func viewerMatchChatQuery(matchID string) []viewerChatLine {
	out := []viewerChatLine{}
	database := currentMmogPlayerStateDB()
	if database == nil || matchID == "" {
		return out
	}
	rows, err := database.Query(`SELECT c.sent_at, c.channel, COALESCE(NULLIF(p.display_name,''), c.sender_id), c.content
		FROM chat_messages c LEFT JOIN player_state p ON p.user_id = c.sender_id
		WHERE c.channel = ? OR (c.channel LIKE ? AND c.channel LIKE '%.team')
		ORDER BY c.sent_at DESC, c.id DESC LIMIT 60`, matchAllChannel(matchID), matchID+"-%")
	if err != nil {
		return out
	}
	defer func() { _ = rows.Close() }()
	for rows.Next() {
		var l viewerChatLine
		var ch string
		if rows.Scan(&l.At, &ch, &l.Name, &l.Text) == nil {
			l.Team = strings.HasSuffix(ch, ".team")
			out = append(out, l)
		}
	}
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return out
}

func adminAPIBattles(w http.ResponseWriter, _ *http.Request) {
	writeAdminJSON(w, map[string]any{"battles": viewerBattles(false)})
}

func adminAPIBattle(w http.ResponseWriter, r *http.Request) {
	id := mux.Vars(r)["id"]
	for _, b := range viewerBattles(true) {
		if b.BattleMatchID == id {
			writeAdminJSON(w, struct {
				viewerBattle
				Chat  []viewerChatLine  `json:"chat"`
				Score *viewerScoreboard `json:"score,omitempty"`
			}{b, viewerMatchChat(b.MatchID), viewerScoreboardFor(b.BattleMatchID)})
			return
		}
	}
	http.Error(w, `{"error":"no live battle with that id"}`, http.StatusNotFound)
}

// adminAPIBattleCommand runs one chat command (body {"line":"/meteor 20"})
// against this battle only: the command sees the match's All room as its
// channel, so the host commands queue for this battle_match_id. /say still
// posts to Global (the shipped client has no in-match chat window).
func adminAPIBattleCommand(w http.ResponseWriter, r *http.Request) {
	id := mux.Vars(r)["id"]
	var matchID string
	if database := currentMmogPlayerStateDB(); database != nil {
		_ = database.QueryRow(`SELECT id FROM matches WHERE battle_match_id=? ORDER BY created_at DESC LIMIT 1`, id).Scan(&matchID)
	}
	if matchID == "" {
		http.Error(w, `{"error":"that battle has no match record"}`, http.StatusNotFound)
		return
	}
	var req struct {
		Line string `json:"line"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 2048)).Decode(&req); err != nil || strings.TrimSpace(req.Line) == "" {
		http.Error(w, `{"error":"line required"}`, http.StatusBadRequest)
		return
	}
	line := strings.TrimSpace(req.Line)
	if line[0] != '/' && line[0] != '!' {
		line = "/" + line
	}
	reply, ok := handleChatCommand(operatorPID, matchAllChannel(matchID), line)
	if !ok {
		http.Error(w, `{"error":"not a command (or DN_CHAT_COMMANDS=0)"}`, http.StatusBadRequest)
		return
	}
	if database := currentMmogPlayerStateDB(); database != nil {
		adminAudit(database, r, "battle command", id, map[string]any{"match": matchID, "line": line})
	}
	writeAdminJSON(w, map[string]string{"reply": reply})
}

// viewerDBCache holds the database half of a viewer poll for a moment. The
// viewer polls every 100 ms per open tab, and each poll ran the match, roster,
// per-player name and chat queries against a database that takes one
// connection at a time (shared/db MaxOpenConns=1): a profiled tab saw 2 KB
// replies take 400-770 ms while a match was live (Firefox profile 2026-10-17
// 17:42), long enough to run the playback clock past its buffer. The ships
// and shots stay uncached -- they come from memory every poll. GUESS: the
// queue behind other writers is the slow part; the TTLs below are what a
// spectator cannot tell apart (a name or roster a second or two late).
var viewerDBCache = &viewerCache{}

const (
	viewerMetaTTL = 2 * time.Second
	viewerChatTTL = time.Second
	viewerNameTTL = 30 * time.Second
)

type viewerMeta struct {
	at                     time.Time
	matchID, mode, mapPath string
	roster                 int
}

type viewerCache struct {
	mu    sync.Mutex
	metas map[string]viewerMeta
	names map[string]struct {
		at time.Time
		n  string
	}
	chats map[string]struct {
		at    time.Time
		lines []viewerChatLine
	}
}

func (c *viewerCache) meta(battleID string) (viewerMeta, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	m, ok := c.metas[battleID]
	return m, ok && time.Since(m.at) < viewerMetaTTL
}

func (c *viewerCache) putMeta(battleID string, m viewerMeta) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.metas == nil {
		c.metas = map[string]viewerMeta{}
	}
	for k, v := range c.metas {
		if time.Since(v.at) > time.Minute {
			delete(c.metas, k)
		}
	}
	m.at = time.Now()
	c.metas[battleID] = m
}

func (c *viewerCache) name(pid string) (string, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.names[pid]
	return e.n, ok && time.Since(e.at) < viewerNameTTL
}

func (c *viewerCache) putName(pid, n string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.names == nil || len(c.names) > 4096 {
		c.names = map[string]struct {
			at time.Time
			n  string
		}{}
	}
	c.names[pid] = struct {
		at time.Time
		n  string
	}{time.Now(), n}
}

// dropChat forgets a match's cached chat (both viewers' keys) so a line just
// posted shows on the next poll.
func (c *viewerCache) dropChat(matchID string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	for k := range c.chats {
		if k == matchID || strings.HasPrefix(k, matchID+"#") {
			delete(c.chats, k)
		}
	}
}

func (c *viewerCache) chat(matchID string) ([]viewerChatLine, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	e, ok := c.chats[matchID]
	return e.lines, ok && time.Since(e.at) < viewerChatTTL
}

func (c *viewerCache) putChat(matchID string, lines []viewerChatLine) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.chats == nil || len(c.chats) > 256 {
		c.chats = map[string]struct {
			at    time.Time
			lines []viewerChatLine
		}{}
	}
	c.chats[matchID] = struct {
		at    time.Time
		lines []viewerChatLine
	}{time.Now(), lines}
}
