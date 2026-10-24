package main

import (
	"encoding/json"
	"fmt"
	"math/rand"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/matchmaker"
	dreadconfig "github.com/darkace1998/Dreadnought-Revival-project/shared/dreadgameconfig"
	"github.com/gorilla/mux"
	"github.com/sirupsen/logrus"
)

// Custom modes (operator 2026-10-24): a stock mode with host rules on top,
// sent to the match host when the match forms (dn_host_loadout.cpp
// CommandCustom). Played by /match <id>, or, when switched on, in the
// rotation of their base mode: a match queued for the base mode becomes one
// of the switched-on custom modes or the stock one, at random.
// <data>/custom_modes.json, edited in the admin Custom modes tab.

type customDrain struct {
	MinSec int `json:"min_sec"`
	MaxSec int `json:"max_sec"`
	MinDmg int `json:"min_dmg"`
	MaxDmg int `json:"max_dmg"`
}

type customShower struct {
	EverySec int `json:"every_sec"`
	Nukes    int `json:"nukes"`
}

// customWave: fake Havoc -- the bosses of one wave (boss def ids, the admin
// Bosses tab) and how strong.
type customWave struct {
	Bosses []int `json:"bosses"`
	Mult   int   `json:"mult"`
}

type customMode struct {
	ID        string        `json:"id"`
	Label     string        `json:"label"`
	Base      string        `json:"base"` // stock mode the match runs (TDM, Onslaught)
	On        bool          `json:"on"`   // in the base mode's rotation
	FFA       bool          `json:"ffa"`
	Ships     int           `json:"ships"` // bot fill to this many ships in all; 0 = stock
	Teams     int           `json:"teams"` // 3 or 4 sides; 0 = stock two
	Secret    bool          `json:"secret"`
	Scatter   bool          `json:"scatter"`    // every ship spawns at a random start
	KillLimit int           `json:"kill_limit"` // FFA: kills to win; 0 = the mode's
	Drain     *customDrain  `json:"drain,omitempty"`
	Shower    *customShower `json:"shower,omitempty"`
	Waves     []customWave  `json:"waves,omitempty"`
	WaveSec   int           `json:"wave_sec,omitempty"` // fake Havoc: seconds between waves
}

// GUESS: the numbers are the operator's (lifedrain 1-3 s, 10-100 damage;
// 6v6v6, 4v4v4v4; up to 16 ships) or starting points to tune.
var customModeDefaults = []customMode{
	{ID: "ffa", Label: "Free For All", Base: "TDM", FFA: true},
	{ID: "ffa_pve", Label: "Free For All vs AI", Base: "TDM", FFA: true, Ships: 16, Scatter: true},
	{ID: "teams3", Label: "Three Fleets", Base: "TDM", Teams: 3, Ships: 18},
	{ID: "teams4", Label: "Four Fleets", Base: "TDM", Teams: 4, Ships: 16},
	{ID: "secret", Label: "Realistic", Base: "TDM", Secret: true},
	{ID: "lifedrain", Label: "Lifedrain", Base: "TDM", Drain: &customDrain{MinSec: 1, MaxSec: 3, MinDmg: 10, MaxDmg: 100}},
	{ID: "meteors", Label: "Meteor Shower", Base: "TDM", Shower: &customShower{EverySec: 20, Nukes: 16}},
	{ID: "fake_havoc", Label: "Boss Rush", Base: "Onslaught", WaveSec: 120, Waves: []customWave{}},
}

var customStore = struct {
	sync.Mutex
	loaded bool
	modes  []customMode
}{}

func customFile() string { return filepath.Join(dreadconfig.DataDir(), "custom_modes.json") }

func customDefault(id string) (customMode, bool) {
	for _, m := range customModeDefaults {
		if m.ID == id {
			return cloneCustomMode(m), true
		}
	}
	return customMode{}, false
}

func cloneCustomMode(m customMode) customMode {
	raw, _ := json.Marshal(m)
	var c customMode
	_ = json.Unmarshal(raw, &c)
	return c
}

// customModes: the saved modes over the defaults, in default order.
func customModes() []customMode {
	customStore.Lock()
	defer customStore.Unlock()
	if !customStore.loaded {
		customStore.loaded = true
		var saved []customMode
		if raw, err := os.ReadFile(customFile()); err == nil {
			_ = json.Unmarshal(raw, &saved)
		}
		customStore.modes = mergeCustomModes(saved)
	}
	out := make([]customMode, len(customStore.modes))
	for i, m := range customStore.modes {
		out[i] = cloneCustomMode(m)
	}
	return out
}

func mergeCustomModes(saved []customMode) []customMode {
	by := map[string]customMode{}
	for _, m := range saved {
		by[m.ID] = m
	}
	var out []customMode
	for _, d := range customModeDefaults {
		m, ok := by[d.ID]
		if !ok {
			m = cloneCustomMode(d)
		}
		m.ID, m.Label, m.Base = d.ID, d.Label, d.Base // fixed per mode
		out = append(out, m)
	}
	return out
}

func customModeByID(id string) (customMode, bool) {
	for _, m := range customModes() {
		if strings.EqualFold(m.ID, id) {
			return m, true
		}
	}
	return customMode{}, false
}

func (m *customMode) validate() error {
	in := func(v, lo, hi int, nm string) error {
		if v < lo || v > hi {
			return fmt.Errorf("%s: %s must be %d-%d", m.Label, nm, lo, hi)
		}
		return nil
	}
	errs := []error{in(m.Ships, 0, 32, "ships"), in(m.KillLimit, 0, 200, "kill limit")}
	if m.Teams != 0 {
		errs = append(errs, in(m.Teams, 3, 4, "fleets"))
	}
	if d := m.Drain; d != nil {
		errs = append(errs, in(d.MinSec, 1, 60, "drain every"), in(d.MaxSec, d.MinSec, 60, "drain every (most)"),
			in(d.MinDmg, 1, 100000, "drain damage"), in(d.MaxDmg, d.MinDmg, 100000, "drain damage (most)"))
	}
	if s := m.Shower; s != nil {
		errs = append(errs, in(s.EverySec, 5, 600, "shower every"), in(s.Nukes, 1, 40, "nukes"))
	}
	if m.WaveSec != 0 {
		errs = append(errs, in(m.WaveSec, 20, 900, "seconds between waves"))
	}
	for i, w := range m.Waves {
		errs = append(errs, in(w.Mult, 0, 10, fmt.Sprintf("wave %d strength", i+1)), in(len(w.Bosses), 1, 8, fmt.Sprintf("wave %d bosses", i+1)))
		for _, id := range w.Bosses {
			if _, ok := bossByID(id); !ok {
				errs = append(errs, fmt.Errorf("%s: wave %d: no boss %d", m.Label, i+1, id))
			}
		}
	}
	for _, e := range errs {
		if e != nil {
			return e
		}
	}
	return nil
}

// hostLines: what the match host is told when the match forms.
func (m customMode) hostLines() []string {
	var l []string
	if m.FFA {
		l = append(l, fmt.Sprintf("ffa on %d", m.KillLimit))
	}
	if m.Ships > 0 {
		l = append(l, fmt.Sprintf("ships %d", m.Ships))
	}
	if m.Teams >= 3 {
		l = append(l, fmt.Sprintf("teams %d", m.Teams))
	}
	if m.Secret {
		l = append(l, "secret on")
	}
	if m.Scatter {
		l = append(l, "scatter on")
	}
	if d := m.Drain; d != nil {
		l = append(l, fmt.Sprintf("drain %d-%d %d-%d", d.MinSec, d.MaxSec, d.MinDmg, d.MaxDmg))
	}
	if s := m.Shower; s != nil {
		l = append(l, fmt.Sprintf("shower %d %d", s.EverySec, s.Nukes))
	}
	if len(m.Waves) > 0 {
		l = append(l, "creeps off")
	}
	return l
}

// customWaveStart: GUESS: the Onslaught intro and first fly-in take about this
// long before the players can fight.
const customWaveStart = 90 * time.Second

// startCustomMode queues the mode's lines for the match host, and fake
// Havoc's boss waves one by one after it.
func startCustomMode(battleMatchID string, m customMode) {
	for _, line := range m.hostLines() {
		queueStickyHostCommand(battleMatchID, line)
	}
	if len(m.Waves) == 0 {
		return
	}
	every := time.Duration(m.WaveSec) * time.Second
	if every <= 0 {
		every = 2 * time.Minute
	}
	waves := m.Waves
	go func() {
		time.Sleep(customWaveStart)
		for i, w := range waves {
			if i > 0 {
				time.Sleep(every)
			}
			if !battleMatchLive(battleMatchID) {
				return
			}
			for _, id := range w.Bosses {
				mult := w.Mult
				if mult <= 0 {
					if d, ok := bossByID(id); ok && d.Mult > 0 {
						mult = d.Mult
					} else {
						mult = 1
					}
				}
				queueHostCommand(battleMatchID, fmt.Sprintf("bossdef %d %d", id, mult))
			}
			logrus.WithField("match", battleMatchID).Infof("custom mode %s: wave %d of %d, %d boss(es)", m.ID, i+1, len(waves), len(w.Bosses))
		}
	}()
}

// customRotationPick: for a match of a stock mode not pinned to anything,
// one of the switched-on custom modes of that base, or none (the stock mode),
// each equally likely.
func customRotationPick(launchMode string) (customMode, bool) {
	var on []customMode
	for _, m := range customModes() {
		if m.On && strings.EqualFold(m.Base, launchMode) {
			on = append(on, m)
		}
	}
	if len(on) == 0 {
		return customMode{}, false
	}
	stock := 0
	if matchmaker.ModeEnabled(launchMode) {
		stock = 1
	}
	i := rand.Intn(len(on) + stock) //nolint:gosec // a mode pick
	if i >= len(on) {
		return customMode{}, false
	}
	return on[i], true
}

func init() {
	matchmaker.OnMatchFormed = func(battleMatchID, launchMode, custom string, pinned bool) {
		if battleMatchID == "" {
			return
		}
		var m customMode
		var ok bool
		if custom != "" {
			m, ok = customModeByID(custom)
		} else if !pinned {
			m, ok = customRotationPick(launchMode)
		}
		if ok {
			logrus.WithFields(logrus.Fields{"match": battleMatchID, "mode": m.ID, "lines": m.hostLines()}).Info("custom mode for the match")
			startCustomMode(battleMatchID, m)
		}
	}
}

func saveCustomModes(in []customMode) ([]customMode, error) {
	modes := mergeCustomModes(in)
	for i := range modes {
		if err := modes[i].validate(); err != nil {
			return nil, err
		}
	}
	raw, _ := json.MarshalIndent(modes, "", "  ")
	tmp := customFile() + ".tmp"
	if err := os.WriteFile(tmp, append(raw, '\n'), 0o644); err != nil {
		return nil, err
	}
	if err := os.Rename(tmp, customFile()); err != nil {
		return nil, err
	}
	customStore.Lock()
	customStore.modes, customStore.loaded = modes, true
	customStore.Unlock()
	return modes, nil
}

// customBossChoices: the boss defs a wave can name, by name.
func customBossChoices() []map[string]any {
	var out []map[string]any
	for _, d := range bossList() {
		out = append(out, map[string]any{"id": d.ID, "name": d.Name, "tier": d.Tier})
	}
	sort.Slice(out, func(i, j int) bool { return out[i]["name"].(string) < out[j]["name"].(string) })
	return out
}

func registerAdminCustomModes(api *mux.Router) {
	view := func() map[string]any {
		return map[string]any{"modes": customModes(), "defaults": customModeDefaults, "bosses": customBossChoices()}
	}
	api.HandleFunc("/custom-modes", func(w http.ResponseWriter, _ *http.Request) {
		writeAdminJSON(w, view())
	}).Methods(http.MethodGet)
	api.HandleFunc("/custom-modes", func(w http.ResponseWriter, r *http.Request) {
		var in struct {
			Modes []customMode `json:"modes"`
		}
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<16)).Decode(&in); err != nil {
			adminBossError(w, http.StatusBadRequest, "bad JSON: "+err.Error())
			return
		}
		modes, err := saveCustomModes(in.Modes)
		if err != nil {
			adminBossError(w, http.StatusBadRequest, err.Error())
			return
		}
		if db := currentMmogPlayerStateDB(); db != nil {
			adminAudit(db, r, "save custom modes", "", map[string]any{"modes": modes})
		}
		writeAdminJSON(w, view())
	}).Methods(http.MethodPost, http.MethodPut)
}

// battleMatchLive: the match is still running (unknown = yes).
func battleMatchLive(battleMatchID string) bool {
	database := currentMmogPlayerStateDB()
	if database == nil {
		return true
	}
	var n int
	if err := database.QueryRow(`SELECT count(*) FROM matches WHERE battle_match_id=? AND status='active'`, battleMatchID).Scan(&n); err != nil {
		return true
	}
	return n > 0
}

func customModeIDs() []string {
	var ids []string
	for _, m := range customModeDefaults {
		ids = append(ids, m.ID)
	}
	return ids
}
