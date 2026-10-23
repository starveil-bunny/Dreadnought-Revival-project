package main

// Data for the launcher's "dreadnought" skin (dn-launcher): who is online and
// what is being played, before the player starts the game, plus their own
// account card. Reached through the gateway's mmog.* route like everything
// else on :8083. /launcher/status is public (counts and mode/map names only,
// no player names); /launcher/me needs the launcher's own JWT
// (aud "launcher", jwtMiddlewareFor).

import (
	"bytes"
	"compress/zlib"
	"database/sql"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/handlers"
	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/protocol"
	dreadconfig "github.com/darkace1998/Dreadnought-Revival-project/shared/dreadgameconfig"
)

// launcherUID is the identity both launcher handlers key their rows by: the
// 32-hex player id player_state.user_id and player_save_blobs.user_id use (the
// auth user id with the hyphens stripped — see normalizedPlayerStatePID).
//
// It returns "" for anything that is not a real id, and the callers reject
// that with 401 rather than falling through to normalizedPlayerStatePID: that
// helper substitutes defaultMmogPlayerPID when normalization fails, so a token
// whose user_id is not a UUID would silently read — and, in
// launcherSkipTutorialHandler, overwrite — the template player's rows instead
// of failing. This handler takes its identity from the launcher's JWT rather
// than the binary protocol, so it is the one place a non-UUID id can arrive.
func launcherUID(r *http.Request) string {
	return protocol.NormalizePlayerPID(r.Header.Get("X-User-ID"))
}

type launcherBattle struct {
	Mode    string `json:"mode"`
	Map     string `json:"map"`
	Players int    `json:"players"`
}

func launcherStatusHandler(w http.ResponseWriter, _ *http.Request) {
	online := 0
	if socialHubInstance != nil {
		online = len(socialHubInstance.onlinePlayers())
	}
	battles := []launcherBattle{}
	for _, b := range viewerBattles(false) {
		battles = append(battles, launcherBattle{Mode: b.ModeName, Map: b.MapName, Players: b.Players})
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	_ = json.NewEncoder(w).Encode(map[string]any{"online": online, "battles": battles, "viewer": launcherViewerEnabled()})
}

type launcherAccount struct {
	Name string `json:"name"`
	// Title is the player's rank title, the name the client itself puts on a
	// player (dreadconfig.RankTitle: m_rankName of DN_Ranks_Player, the table
	// UI_Persistent_PlayerCard renders as RankTitle). Left out entirely when
	// the rank has no name -- the card shows nothing rather than a made-up one.
	Title string `json:"title,omitempty"`
	Rank  int    `json:"rank"`
	// XP is progress within the rank (player_state.rank_xp, see
	// grantBattleRewards); Next the XP the next rank takes, 0 at the top.
	XP      int64 `json:"xp"`
	Next    int64 `json:"next"`
	Credits int64 `json:"credits"`
	Gold    int64 `json:"gold"`
	FreeXP  int64 `json:"freexp"`
	// InBattle: in a live match now (the launcher offers its battle view).
	InBattle bool `json:"in_battle,omitempty"`
	Matches  int  `json:"matches"`
	Wins     int  `json:"wins"`
	Kills    int  `json:"kills"`
	Deaths   int  `json:"deaths"`
	// Tutorial: the onboarding blob says the tutorial is finished (or was
	// skipped), so the launcher shows that instead of the skip button.
	Tutorial bool `json:"tutorial"`
	// FleetTier: the highest fleet the client would show unlocked
	// (unlockedFleets: 1 Recruit, 2 Veteran, 3 Legendary); the FLEET button's
	// frame and badge.
	FleetTier int32 `json:"fleet_tier,omitempty"`
	Recent    []struct {
		Outcome string `json:"outcome"`
		Kills   int    `json:"kills"`
		Deaths  int    `json:"deaths"`
		Mode    string `json:"mode"`
		Map     string `json:"map"`
	} `json:"recent"`
}

func launcherMeHandler(w http.ResponseWriter, r *http.Request) {
	uid := launcherUID(r)
	if uid == "" {
		http.Error(w, `{"error":"invalid user id"}`, http.StatusUnauthorized)
		return
	}
	database := currentMmogPlayerStateDB()
	if database == nil {
		http.Error(w, `{"error":"unavailable"}`, http.StatusServiceUnavailable)
		return
	}
	var a launcherAccount
	if err := database.QueryRow(`SELECT display_name, current_rank, rank_xp, soft_currency, premium_currency, free_xp
		FROM player_state WHERE user_id=?`, uid).Scan(&a.Name, &a.Rank, &a.XP, &a.Credits, &a.Gold, &a.FreeXP); err != nil {
		// No hangar visit yet: the card shows the callsign only.
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{"new": true, "tutorial": tutorialFinished(database, uid)})
		return
	}
	a.Tutorial = tutorialFinished(database, uid)
	_, _, a.InBattle = activeMatchSlot(uid)
	a.Title = dreadconfig.RankTitle(int32(a.Rank))
	if st := mmogPlayerStateForPID(uid); len(st.fleets) > 0 {
		for _, f := range unlockedFleets(st, uid, st.fleets) {
			if f.fleetType > a.FleetTier && f.fleetType <= 3 {
				a.FleetTier = f.fleetType
			}
		}
	}
	if t := handlers.RankXPThreshold(int32(a.Rank) + 1); t > 0 {
		a.Next = int64(t)
	}
	_ = database.QueryRow(`SELECT count(*), coalesce(sum(outcome='win'),0), coalesce(sum(kills),0), coalesce(sum(deaths),0)
		FROM battle_results WHERE user_id=?`, uid).Scan(&a.Matches, &a.Wins, &a.Kills, &a.Deaths)
	rows, err := database.Query(`SELECT b.outcome, b.kills, b.deaths,
		coalesce((SELECT game_mode FROM matches WHERE battle_match_id=b.match_id ORDER BY created_at DESC LIMIT 1),''),
		coalesce((SELECT map FROM matches WHERE battle_match_id=b.match_id ORDER BY created_at DESC LIMIT 1),'')
		FROM battle_results b WHERE b.user_id=? ORDER BY b.created_at DESC LIMIT 5`, uid)
	if err == nil {
		for rows.Next() {
			var e struct {
				Outcome string `json:"outcome"`
				Kills   int    `json:"kills"`
				Deaths  int    `json:"deaths"`
				Mode    string `json:"mode"`
				Map     string `json:"map"`
			}
			if rows.Scan(&e.Outcome, &e.Kills, &e.Deaths, &e.Mode, &e.Map) == nil {
				e.Mode, e.Map = viewerModeName(e.Mode), viewerMapName(e.Map)
				a.Recent = append(a.Recent, e)
			}
		}
		_ = rows.Close()
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	_ = json.NewEncoder(w).Encode(a)
}

// tutorialFinished reads the login gate's own test out of the account's SGD
// blob: the Ob_TutorialFinished rule's m_lastFired DateTime is non-zero. The
// blob is a little-endian uncompressed size then zlib; the DateTime value is
// an int64 of ticks after the StructProperty header that ends "DateTime\0",
// a 16-byte struct GUID and a 1-byte property-GUID flag (read off the three
// stored blobs, 2026-10-17).
func tutorialFinished(database *sql.DB, uid string) bool {
	var blob []byte
	if database.QueryRow(`SELECT data FROM player_save_blobs WHERE user_id=? AND slot=?`,
		uid, playerSaveSlotOnboarding).Scan(&blob) != nil || len(blob) < 4 {
		return false
	}
	zr, err := zlib.NewReader(bytes.NewReader(blob[4:]))
	if err != nil {
		return false
	}
	d, err := io.ReadAll(io.LimitReader(zr, 1<<20))
	if err != nil && len(d) == 0 {
		return false
	}
	i := bytes.Index(d, []byte("Ob_TutorialFinished\x00"))
	if i < 0 {
		return false
	}
	j := bytes.Index(d[i:], []byte("DateTime\x00"))
	if j < 0 {
		return false
	}
	at := i + j + len("DateTime\x00") + 16 + 1
	if at+8 > len(d) {
		return false
	}
	return binary.LittleEndian.Uint64(d[at:]) != 0
}

// launcherSkipTutorialHandler: POST /launcher/skip-tutorial (launcher JWT).
// Gives the account the DN_ONBOARDING_TEMPLATE_PID player's onboarding SGD
// blob, the same one a brand-new account is handed (onboardingTemplateBlob):
// its Ob_TutorialFinished rule is what the login gate checks. Replaces the
// account's own blob, so other onboarding rule progress goes with it -- the
// player asked for that by pressing the button. Takes effect at next login.
func launcherSkipTutorialHandler(w http.ResponseWriter, r *http.Request) {
	uid := launcherUID(r)
	if uid == "" {
		http.Error(w, `{"error":"invalid user id"}`, http.StatusUnauthorized)
		return
	}
	database := currentMmogPlayerStateDB()
	if database == nil {
		http.Error(w, `{"error":"unavailable"}`, http.StatusServiceUnavailable)
		return
	}
	blob := onboardingTemplateBlob(database)
	if blob == nil {
		http.Error(w, `{"error":"no tutorial template on this server"}`, http.StatusNotImplemented)
		return
	}
	if _, err := database.Exec(`INSERT INTO player_save_blobs(user_id, slot, data, updated_at)
		VALUES(?, ?, ?, datetime('now'))
		ON CONFLICT(user_id, slot) DO UPDATE SET data=excluded.data, updated_at=excluded.updated_at`,
		uid, playerSaveSlotOnboarding, blob); err != nil {
		http.Error(w, `{"error":"save failed"}`, http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write([]byte(`{"ok":true}` + "\n"))
}
