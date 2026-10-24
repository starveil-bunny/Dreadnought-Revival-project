package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"

	dreadconfig "github.com/darkace1998/Dreadnought-Revival-project/shared/dreadgameconfig"
	"github.com/gorilla/mux"
)

// Boats, fighters and waves (operator 2026-10-22: "a config page for the
// boats and fighters ... 24 enemies was straining it"): the counts /squad
// (Onslaught assault ships, VH_ONS_AITargetM), /swarm (Onslaught fighters,
// VH_ONS_AITargetL), /hunters and /wave use when none is typed, the caps a
// typed count may not pass, and the team they join. <data>/waves/config.json,
// edited in the admin Waves tab. The battle host keeps its own hard caps
// (60 fighters, 20 assault ships, 12 hunters).

type waveConfig struct {
	SwarmDefault   int `json:"swarm_default"`
	SwarmMax       int `json:"swarm_max"`
	SquadDefault   int `json:"squad_default"`
	SquadMax       int `json:"squad_max"`
	HuntersDefault int `json:"hunters_default"` // per player
	Team           int `json:"team"`            // /swarm and /squad: 3 = purple, fights everyone
	WaveSquad      int `json:"wave_squad"`
	WaveSwarm      int `json:"wave_swarm"`
	WaveHunters    int `json:"wave_hunters"` // in all, spread over the players
}

// GUESS: defaults. The swarm was 24 and the operator saw the server strain
// with /wave's 24 fighters + 8 assault ships + a boss, so the wave halves the
// fighters until the stutter log says what costs what.
var waveDefaults = waveConfig{SwarmDefault: 24, SwarmMax: 60, SquadDefault: 8, SquadMax: 20, HuntersDefault: 2, Team: 3,
	WaveSquad: 6, WaveSwarm: 12, WaveHunters: 4}

var waveStore = struct {
	sync.Mutex
	loaded bool
	c      waveConfig
}{}

func waveFile() string { return filepath.Join(dreadconfig.DataDir(), "waves", "config.json") }

func waveCfg() waveConfig {
	waveStore.Lock()
	defer waveStore.Unlock()
	if !waveStore.loaded {
		waveStore.loaded = true
		waveStore.c = waveDefaults
		if raw, err := os.ReadFile(waveFile()); err == nil {
			_ = json.Unmarshal(raw, &waveStore.c)
		}
	}
	return waveStore.c
}

func (c *waveConfig) validate() error {
	for _, f := range []struct {
		v      int
		lo, hi int
		nm     string
	}{
		{c.SwarmMax, 1, 60, "swarm max"}, {c.SwarmDefault, 1, c.SwarmMax, "swarm default"},
		{c.SquadMax, 1, 20, "squad max"}, {c.SquadDefault, 1, c.SquadMax, "squad default"},
		{c.HuntersDefault, 1, 12, "hunters per player"}, {c.Team, 0, 4, "team"},
		{c.WaveSquad, 0, c.SquadMax, "wave squad"}, {c.WaveSwarm, 0, c.SwarmMax, "wave swarm"}, {c.WaveHunters, 0, 12, "wave hunters"},
	} {
		if f.v < f.lo || f.v > f.hi {
			return fmt.Errorf("%s must be %d-%d", f.nm, f.lo, f.hi)
		}
	}
	return nil
}

func registerAdminWaves(api *mux.Router) {
	api.HandleFunc("/waves", func(w http.ResponseWriter, _ *http.Request) {
		writeAdminJSON(w, map[string]any{"config": waveCfg(), "defaults": waveDefaults})
	}).Methods(http.MethodGet)
	api.HandleFunc("/waves", func(w http.ResponseWriter, r *http.Request) {
		var c waveConfig
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&c); err != nil {
			adminBossError(w, http.StatusBadRequest, "bad JSON: "+err.Error())
			return
		}
		if err := c.validate(); err != nil {
			adminBossError(w, http.StatusBadRequest, err.Error())
			return
		}
		raw, _ := json.MarshalIndent(c, "", "  ")
		if err := os.MkdirAll(filepath.Dir(waveFile()), 0o755); err == nil {
			tmp := waveFile() + ".tmp"
			if err = os.WriteFile(tmp, append(raw, '\n'), 0o644); err == nil {
				err = os.Rename(tmp, waveFile())
			}
			if err != nil {
				adminBossError(w, http.StatusInternalServerError, err.Error())
				return
			}
		}
		waveStore.Lock()
		waveStore.c, waveStore.loaded = c, true
		waveStore.Unlock()
		if db := currentMmogPlayerStateDB(); db != nil {
			adminAudit(db, r, "save wave config", "", map[string]any{"config": c})
		}
		writeAdminJSON(w, map[string]any{"config": c, "defaults": waveDefaults})
	}).Methods(http.MethodPost, http.MethodPut)
	api.HandleFunc("/stutters", adminAPIStutters).Methods(http.MethodGet)
}

// ---- stutter log --------------------------------------------------------

// The battle host logs one "stutter:" line per game-thread frame gap over
// its threshold (dn_host_loadout.cpp FrameTick): the gap, what of ours ran
// last and how long it took, the last command and its age, and live counts.
// adminAPIStutters reads them back from the mod log and totals them by the
// suspect (our zone when it overlapped the gap, else the last command when
// recent, else "engine").
var stutterLine = regexp.MustCompile(`^\[dn-host-loadout\] (\S+) stutter: (\S+) (\d+) ms gap\b(.*)$`)
var stutterZoneLine = regexp.MustCompile(`^\[dn-host-loadout\] \S+ stutter-zone: (\S+) took (\d+) ms`)
var stutterKV = regexp.MustCompile(`(\w+)=("[^"]*"|\S+)`)

type stutterEvent struct {
	Time    string            `json:"time"`
	Match   string            `json:"match"`
	Ms      int               `json:"ms"`
	Suspect string            `json:"suspect"`
	Fields  map[string]string `json:"fields"`
}

func adminAPIStutters(w http.ResponseWriter, r *http.Request) {
	path, ok := adminLogPath("mod", "")
	if !ok {
		writeAdminJSON(w, map[string]any{"events": []stutterEvent{}, "error": "no mod log (GAME_BINARY unset)"})
		return
	}
	limit, _ := strconv.Atoi(r.URL.Query().Get("n"))
	if limit <= 0 || limit > 2000 {
		limit = 400
	}
	text := tailFile(path, 8*1024*1024)
	var events []stutterEvent
	type agg struct {
		Suspect string `json:"suspect"`
		Count   int    `json:"count"`
		TotalMs int    `json:"total_ms"`
		WorstMs int    `json:"worst_ms"`
	}
	by, zones := map[string]*agg{}, map[string]*agg{}
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimRight(line, "\r")
		if z := stutterZoneLine.FindStringSubmatch(line); z != nil {
			ms, _ := strconv.Atoi(z[2])
			a := zones[z[1]]
			if a == nil {
				a = &agg{Suspect: z[1]}
				zones[z[1]] = a
			}
			a.Count++
			a.TotalMs += ms
			a.WorstMs = max(a.WorstMs, ms)
			continue
		}
		m := stutterLine.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		ms, _ := strconv.Atoi(m[3])
		e := stutterEvent{Time: m[1], Match: m[2], Ms: ms, Fields: map[string]string{}}
		for _, kv := range stutterKV.FindAllStringSubmatch(m[4], -1) {
			e.Fields[kv[1]] = strings.Trim(kv[2], `"`)
		}
		e.Suspect = e.Fields["suspect"]
		if e.Suspect == "" {
			e.Suspect = "engine"
		}
		events = append(events, e)
		a := by[e.Suspect]
		if a == nil {
			a = &agg{Suspect: e.Suspect}
			by[e.Suspect] = a
		}
		a.Count++
		a.TotalMs += ms
		a.WorstMs = max(a.WorstMs, ms)
	}
	if len(events) > limit {
		events = events[len(events)-limit:]
	}
	sorted := func(m map[string]*agg) []*agg {
		out := make([]*agg, 0, len(m))
		for _, a := range m {
			out = append(out, a)
		}
		sort.Slice(out, func(i, j int) bool { return out[i].TotalMs > out[j].TotalMs })
		return out
	}
	if events == nil {
		events = []stutterEvent{}
	}
	writeAdminJSON(w, map[string]any{"events": events, "by_suspect": sorted(by), "slow_zones": sorted(zones)})
}

// ---- game art (scripts/gen-admin-art.py) --------------------------------

var adminArtName = regexp.MustCompile(`^(ships|ui|previews|icons/abilities|icons/weapons|icons/feats|icons/perks|icons/markers)/[A-Za-z0-9_-]+\.png$|` +
	`^models/[A-Za-z0-9_-]+\.(glb|png|jpg)$|^maps/[A-Za-z0-9_-]+\.jpg$|^modes/[A-Za-z0-9_-]+\.png$|^vendor/[A-Za-z0-9_.-]+\.js$|^vanity/[A-Za-z0-9_-]+\.png$|^art\.json$`)

// handleAdminArt serves data/admin-art (and the decoded cosmetic pictures in
// data/market-images as vanity/<name>). Client art and stats, nothing
// private, so outside the admin key like the viewer icons: an <img> cannot
// send the key header.
func handleAdminArt(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, "/admin/art/")
	if !adminArtName.MatchString(name) {
		http.NotFound(w, r)
		return
	}
	file := filepath.Join(dreadconfig.DataDir(), "admin-art", filepath.FromSlash(name))
	if rest, ok := strings.CutPrefix(name, "vanity/"); ok {
		file = filepath.Join(dreadconfig.MarketImagesDir(), rest)
	}
	if name == "art.json" {
		w.Header().Set("Cache-Control", "no-cache") // regenerated with the art; revalidate (ETag/Last-Modified)
	} else {
		w.Header().Set("Cache-Control", "public, max-age=3600")
	}
	http.ServeFile(w, r, file)
}
