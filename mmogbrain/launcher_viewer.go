package main

// Battle viewer in the launcher (dn-launcher), and the scoreboard and kill feed
// both viewers show.
//
// Scores: the host appends "t kills1 kills2 points1 points2", one "s team kills
// deaths assists score bot pid name" per player state, and the recent kills as
// "k seq kteam vteam kclass vclass kpid vpid kname vname" to a snapshot once a
// second (battle-server-mod ViewerScoreLines). The latest scoreboard is kept
// per battle; kills are appended by seq into a per-battle log.
//
// Launcher viewer: any signed-in player can watch any live battle and chat in
// its All room (a "ghost"); in their own battle they also get their Team chat.
// No commands. Off unless DN_LAUNCHER_VIEWER=1; /launcher/status says
// whether it is on so the launcher can offer it. The page is the admin viewer
// with window.DN_PLAYER set (launcherViewerPage); the launcher shows it in a
// frame and proxies its requests (the server's certificate is our own CA,
// which the launcher's Go client trusts and the web view may not).

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"io"
	"math"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/protocol"
)

func launcherViewerEnabled() bool { return os.Getenv("DN_LAUNCHER_VIEWER") == "1" }

// launcherViewerSight: how far from one of their team's ships a player is shown
// an enemy ship or shot, in world units; 0 (the default) shows every enemy. The
// stock client draws every enemy actor and gates only the marker, on team and
// cloak (AYPlayerController_TER::OnSetEnemySpotted 0x5BD5E0, DeepSeek report
// 2026-10-17); it has no distance gate. DN_LAUNCHER_VIEWER_SIGHT sets one.
// Cloaked enemies are hidden from players either way (launcherViewerFilter).
func launcherViewerSight() float64 {
	if v, err := strconv.ParseFloat(os.Getenv("DN_LAUNCHER_VIEWER_SIGHT"), 64); err == nil && v >= 0 {
		return v
	}
	return 0
}

type viewerScore struct {
	Team    int    `json:"team"`
	Kills   int    `json:"kills"`
	Deaths  int    `json:"deaths"`
	Assists int    `json:"assists"`
	Score   int    `json:"score"`
	Bot     bool   `json:"bot"`
	PID     string `json:"-"`
	Name    string `json:"name"`
}

type viewerKill struct {
	Seq        int    `json:"seq"`
	At         int64  `json:"at"` // unix ms, when mmogbrain first saw it
	KillerTeam int    `json:"kt"`
	VictimTeam int    `json:"vt"`
	Killer     string `json:"killer"` // "" for no killer (crash, boundary, self)
	Victim     string `json:"victim"`
	KillerKind string `json:"kkind,omitempty"`
	VictimKind string `json:"vkind,omitempty"`
	KillerShip string `json:"kship,omitempty"`
	VictimShip string `json:"vship,omitempty"`
	killerPID  string
	victimPID  string
}

type viewerScoreboard struct {
	at      time.Time
	Kills   [2]int        `json:"kills"`
	Points  [2]int        `json:"points"` // -1 outside Onslaught
	Players []viewerScore `json:"players"`
	Feed    []viewerKill  `json:"feed"`
}

const viewerKillLogMax = 100

var viewerScores struct {
	mu     sync.Mutex
	boards map[string]*viewerScoreboard // battle_match_id
}

// parseViewerScores reads the "t", "s" and "k" lines of a snapshot. have is
// false when the snapshot carried none (the host sends them once a second).
func parseViewerScores(body io.Reader) (teams [4]int, players []viewerScore, kills []viewerKill, have bool) {
	sc := bufio.NewScanner(body)
	for sc.Scan() {
		f := strings.Fields(sc.Text())
		if len(f) == 0 {
			continue
		}
		atoi := func(s string) int { v, _ := strconv.Atoi(s); return v }
		name := func(s string) string {
			if s == "-" {
				return ""
			}
			if n, err := url.PathUnescape(s); err == nil {
				return n
			}
			return s
		}
		pid := func(s string) string {
			if s == "-" {
				return ""
			}
			return protocol.NormalizePlayerPID(s)
		}
		switch {
		case f[0] == "t" && len(f) == 5:
			teams = [4]int{atoi(f[1]), atoi(f[2]), atoi(f[3]), atoi(f[4])}
			have = true
		case f[0] == "s" && len(f) == 9:
			players = append(players, viewerScore{Team: atoi(f[1]), Kills: atoi(f[2]), Deaths: atoi(f[3]), Assists: atoi(f[4]),
				Score: atoi(f[5]), Bot: f[6] == "1", PID: pid(f[7]), Name: name(f[8])})
			have = true
		case f[0] == "k" && len(f) == 10:
			k := viewerKill{Seq: atoi(f[1]), KillerTeam: atoi(f[2]), VictimTeam: atoi(f[3]),
				killerPID: pid(f[6]), victimPID: pid(f[7]), Killer: name(f[8]), Victim: name(f[9])}
			if f[4] != "-" {
				k.KillerKind, k.KillerShip = viewerShipKind(f[4])
			}
			if f[5] != "-" {
				k.VictimKind, k.VictimShip = viewerShipKind(f[5])
			}
			kills = append(kills, k)
			have = true
		}
	}
	return
}

// storeViewerScores keeps a snapshot's scoreboard and adds its new kills.
func storeViewerScores(battleID string, body []byte) {
	teams, players, kills, have := parseViewerScores(bytes.NewReader(body))
	if !have {
		return
	}
	now := time.Now().UnixMilli()
	viewerScores.mu.Lock()
	defer viewerScores.mu.Unlock()
	if viewerScores.boards == nil {
		viewerScores.boards = map[string]*viewerScoreboard{}
	}
	b := viewerScores.boards[battleID]
	if b == nil {
		b = &viewerScoreboard{}
		viewerScores.boards[battleID] = b
	}
	for id, o := range viewerScores.boards {
		if time.Since(o.at) > 10*time.Minute {
			delete(viewerScores.boards, id)
		}
	}
	b.at = time.Now()
	b.Kills = [2]int{teams[0], teams[1]}
	b.Points = [2]int{teams[2], teams[3]}
	b.Players = players
	last := 0
	if n := len(b.Feed); n > 0 {
		last = b.Feed[n-1].Seq
	}
	for _, k := range kills {
		if k.Seq > last {
			k.At = now
			b.Feed = append(b.Feed, k)
			last = k.Seq
		}
	}
	if n := len(b.Feed); n > viewerKillLogMax {
		b.Feed = append([]viewerKill(nil), b.Feed[n-viewerKillLogMax:]...)
	}
}

// viewerScoreboardFor is a copy of the battle's scoreboard with players' names
// from player_state (the host's PlayerName may be the account id), or nil.
func viewerScoreboardFor(battleID string) *viewerScoreboard {
	viewerScores.mu.Lock()
	b := viewerScores.boards[battleID]
	var out *viewerScoreboard
	if b != nil {
		c := *b
		c.Players = append([]viewerScore(nil), b.Players...)
		c.Feed = append([]viewerKill(nil), b.Feed...)
		out = &c
	}
	viewerScores.mu.Unlock()
	if out == nil {
		return nil
	}
	for i := range out.Players {
		if n := viewerPlayerName(out.Players[i].PID); n != "" {
			out.Players[i].Name = n
		}
	}
	for i := range out.Feed {
		if n := viewerPlayerName(out.Feed[i].killerPID); n != "" {
			out.Feed[i].Killer = n
		}
		if n := viewerPlayerName(out.Feed[i].victimPID); n != "" {
			out.Feed[i].Victim = n
		}
		if out.Feed[i].Killer == "" && out.Feed[i].KillerShip != "" {
			out.Feed[i].Killer = out.Feed[i].KillerShip
		}
		if out.Feed[i].Victim == "" {
			out.Feed[i].Victim = out.Feed[i].VictimShip
		}
	}
	return out
}

func viewerPlayerName(pid string) string {
	if pid == "" {
		return ""
	}
	if n, ok := viewerDBCache.name(pid); ok {
		return n
	}
	var n string
	if database := currentMmogPlayerStateDB(); database != nil {
		_ = database.QueryRow(`SELECT COALESCE(display_name,'') FROM player_state WHERE user_id=?`, pid).Scan(&n)
	}
	viewerDBCache.putName(pid, n)
	return n
}

// --- launcher endpoints ------------------------------------------------------

// launcherViewerMatch: the battle a launcher viewer asked for (?battle=), or
// the player's own, or the first one live. Anyone may watch any battle (a
// "ghost": operator 2026-10-17, for someone who cannot play but wants to watch
// and chat). team is the player's team when they are in this battle, 0 for a
// ghost.
func launcherViewerMatch(uid, battleID string) (b *viewerBattle, team int, list []viewerBattle, mine string) {
	list = viewerBattles(true)
	matchID, slotTeam, playing := activeMatchSlot(uid)
	for i := range list {
		if playing && list[i].MatchID == matchID {
			mine = list[i].BattleMatchID
		}
	}
	if battleID == "" {
		battleID = mine
	}
	for i := range list {
		if list[i].BattleMatchID == battleID || (battleID == "" && b == nil) {
			x := list[i]
			b = &x
		}
	}
	if b == nil || b.BattleMatchID != mine {
		return b, 0, list, mine
	}
	team = slotTeam
	for _, s := range b.Ships {
		if s.PID == uid && s.Team > 0 {
			team = s.Team
		}
	}
	return b, team, list, mine
}

// launcherViewerFilter keeps what the player's side would see: every ship of
// their team, and enemy ships and shots within launcherViewerSight of one.
func launcherViewerFilter(b *viewerBattle, team int) {
	if team == 0 { // a ghost sees everything
		return
	}
	ships := b.Ships[:0:0]
	for _, s := range b.Ships {
		if s.Team == team || !s.cloaked {
			ships = append(ships, s)
		}
	}
	b.Ships = ships
	sight := launcherViewerSight()
	if sight <= 0 {
		return
	}
	var allies [][3]float64
	for _, s := range b.Ships {
		if s.Team == team {
			allies = append(allies, [3]float64{s.X, s.Y, s.Z})
		}
	}
	seen := func(x, y, z float64) bool {
		for _, a := range allies {
			if math.Hypot(math.Hypot(a[0]-x, a[1]-y), a[2]-z) <= sight {
				return true
			}
		}
		return false
	}
	ships = b.Ships[:0:0]
	for _, s := range b.Ships {
		if s.Team == team || seen(s.X, s.Y, s.Z) {
			ships = append(ships, s)
		}
	}
	b.Ships = ships
	var proj [][3]float64
	var kinds []byte
	var ids []int32
	for i, p := range b.Projectiles {
		if !seen(p[0], p[1], p[2]) {
			continue
		}
		proj = append(proj, p)
		if i < len(b.ProjKinds) {
			kinds = append(kinds, b.ProjKinds[i])
		}
		if i < len(b.ProjIDs) {
			ids = append(ids, b.ProjIDs[i])
		}
	}
	b.Projectiles, b.ProjKinds, b.ProjIDs = proj, string(kinds), ids
}

func launcherViewerHandler(w http.ResponseWriter, r *http.Request) {
	uid := launcherUID(r)
	if uid == "" {
		http.Error(w, `{"error":"invalid user id"}`, http.StatusUnauthorized)
		return
	}
	if !launcherViewerEnabled() {
		writeAdminJSON(w, map[string]any{"enabled": false})
		return
	}
	b, team, list, mine := launcherViewerMatch(uid, r.URL.Query().Get("battle"))
	type tab struct {
		ID       string `json:"battle_match_id"`
		ModeName string `json:"mode_name"`
		MapName  string `json:"map_name"`
		Mode     string `json:"mode,omitempty"`
		Map      string `json:"map,omitempty"`
		Mine     bool   `json:"mine,omitempty"`
	}
	tabs := []tab{}
	for _, x := range list {
		tabs = append(tabs, tab{x.BattleMatchID, x.ModeName, x.MapName, x.Mode, x.Map, x.BattleMatchID == mine})
	}
	if b == nil {
		writeAdminJSON(w, map[string]any{"enabled": true, "battle": nil, "battles": tabs})
		return
	}
	launcherViewerFilter(b, team)
	for i := range b.Ships {
		b.Ships[i].Me = b.Ships[i].PID == uid
		b.Ships[i].PID = "" // other players' ids stay on the server
	}
	writeAdminJSON(w, map[string]any{"enabled": true, "team": team, "battles": tabs, "battle": struct {
		viewerBattle
		Chat  []viewerChatLine  `json:"chat"`
		Score *viewerScoreboard `json:"score,omitempty"`
	}{*b, viewerMatchChatFor(b.MatchID, team), viewerScoreboardFor(b.BattleMatchID)}})
}

func launcherViewerTerrainHandler(w http.ResponseWriter, r *http.Request) {
	uid := launcherUID(r)
	if uid == "" || !launcherViewerEnabled() {
		http.Error(w, `{"error":"unavailable"}`, http.StatusNotFound)
		return
	}
	b, _, _, _ := launcherViewerMatch(uid, r.URL.Query().Get("battle"))
	var t *viewerTerrain
	if b != nil {
		t = viewerTerrainFor(b.BattleMatchID)
	}
	if t == nil {
		http.Error(w, `{"error":"no terrain for your battle"}`, http.StatusNotFound)
		return
	}
	writeAdminJSON(w, t)
}

// launcherViewerChatHandler posts {"text": ..., "team": bool, "battle": id}
// into that battle's All room (or the player's Team room, when they are in
// it) as that player, as if typed in game (persisted, filtered, sent to
// the room). Commands are refused: the launcher viewer has no controls.
func launcherViewerChatHandler(w http.ResponseWriter, r *http.Request) {
	uid := launcherUID(r)
	if uid == "" || !launcherViewerEnabled() {
		http.Error(w, `{"error":"unavailable"}`, http.StatusNotFound)
		return
	}
	var req struct {
		Text   string `json:"text"`
		Team   bool   `json:"team"`
		Battle string `json:"battle"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 2048)).Decode(&req); err != nil {
		http.Error(w, `{"error":"bad request"}`, http.StatusBadRequest)
		return
	}
	text := strings.TrimSpace(req.Text)
	if text == "" || len(text) > 200 {
		http.Error(w, `{"error":"1-200 characters"}`, http.StatusBadRequest)
		return
	}
	if text[0] == '/' || text[0] == '!' {
		http.Error(w, `{"error":"commands are not available here"}`, http.StatusBadRequest)
		return
	}
	b, team, _, _ := launcherViewerMatch(uid, req.Battle)
	if b == nil || b.MatchID == "" {
		http.Error(w, `{"error":"that battle is over"}`, http.StatusNotFound)
		return
	}
	matchID := b.MatchID
	room := matchAllChannel(matchID)
	if req.Team {
		if team == 0 {
			http.Error(w, `{"error":"team chat is for players in this battle"}`, http.StatusBadRequest)
			return
		}
		room = matchTeamChannel(matchID, team)
	}
	socialHubInstance.postAsPlayer(socialID(uid), room, text)
	viewerDBCache.dropChat(matchID)
	writeAdminJSON(w, map[string]bool{"ok": true})
}

// postAsPlayer sends one chat line into a room from a player who may not be
// connected there (the launcher), the way chat.channel.message does for one
// who is.
func (h *socialHub) postAsPlayer(pid, room, body string) {
	persistMmogChatMessage(pid, room, body)
	notice := chatMessageNotice("chat.channel.message", room, h.presenceEntry(pid), body)
	others := withChatFilter(notice, body, false)
	for _, p := range h.peersIn(room) {
		if p.playerID != pid && h.ignores(p.playerID, pid) {
			continue
		}
		_ = p.send(others)
	}
}

// viewerMatchChatFor: the match's All room and one team's room (none for team
// 0), newest 60.
func viewerMatchChatFor(matchID string, team int) []viewerChatLine {
	key := matchID + "#" + strconv.Itoa(team)
	if c, ok := viewerDBCache.chat(key); ok {
		return c
	}
	out := []viewerChatLine{}
	database := currentMmogPlayerStateDB()
	if database != nil && matchID != "" {
		rows, err := database.Query(`SELECT c.sent_at, c.channel, COALESCE(NULLIF(p.display_name,''), c.sender_id), c.content
			FROM chat_messages c LEFT JOIN player_state p ON p.user_id = c.sender_id
			WHERE c.channel IN (?, ?) ORDER BY c.sent_at DESC, c.id DESC LIMIT 60`,
			matchAllChannel(matchID), matchTeamChannel(matchID, team))
		if err == nil {
			for rows.Next() {
				var l viewerChatLine
				var ch string
				if rows.Scan(&l.At, &ch, &l.Name, &l.Text) == nil {
					l.Team = strings.HasSuffix(ch, ".team")
					out = append(out, l)
				}
			}
			_ = rows.Close()
		}
	}
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	viewerDBCache.putChat(key, out)
	return out
}

// launcherViewerPage: the viewer page in player mode, with the ship icons
// inline (the launcher's frame has no server origin to load them from).
func launcherViewerPage(w http.ResponseWriter, r *http.Request) {
	if launcherUID(r) == "" || !launcherViewerEnabled() {
		http.Error(w, "unavailable", http.StatusNotFound)
		return
	}
	page := adminViewerHTML
	if p := os.Getenv("DN_VIEWER_HTML"); p != "" {
		if b, err := os.ReadFile(p); err == nil {
			page = b
		}
	}
	icons := map[string]string{}
	if entries, err := viewerIcons.ReadDir("viewer_icons"); err == nil {
		for _, e := range entries {
			if b, err := viewerIcons.ReadFile("viewer_icons/" + e.Name()); err == nil && strings.HasSuffix(e.Name(), ".png") {
				icons[strings.TrimSuffix(e.Name(), ".png")] = "data:image/png;base64," + base64.StdEncoding.EncodeToString(b)
			}
		}
	}
	cfg, _ := json.Marshal(map[string]any{"icons": icons})
	inject := []byte("<script>window.DN_PLAYER = " + string(cfg) + ";</script>\n<script>")
	page = bytes.Replace(page, []byte("<script>"), inject, 1)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write(page)
}
