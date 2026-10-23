package main

import (
	"encoding/json"
	"fmt"
	"math/rand"
	"net"
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

// Hunter pool (operator 2026-10-22: "hunters is fine but we need them to be
// pulled from a better curated pool"): /hunters used to fly a random
// class x weight T5 hull with its AI row's modules. Now each hunter is a
// definition shaped like a custom boss (base, name, modules, coatings, feats,
// multiplier) plus a pick weight, edited in the admin Hunters tab and kept in
// <data>/hunters/hunters.json. The battle host fetches its picks in one
// loopback request, GET /battle/hunters?n=K, and spawns them as elites that
// skip the boss treatment (team 3, marker, arrival flash, gold paint).
//
// With no file yet the pool is seeded with one hunter per hull line: the T5
// Havoc base of that line that has a T5 AI pawn, its stock modules, named
// after the line's T5 player hull.

type hunterDef struct {
	bossDef
	Weight int `json:"weight"` // relative pick chance, 0 = benched
}

var hunterStore = struct {
	sync.Mutex
	loaded bool
	defs   map[int]hunterDef
}{}

func hunterFile() string { return filepath.Join(dreadconfig.DataDir(), "hunters", "hunters.json") }

func hunterLoadLocked() {
	if hunterStore.loaded {
		return
	}
	hunterStore.loaded = true
	hunterStore.defs = map[int]hunterDef{}
	raw, err := os.ReadFile(hunterFile())
	if err != nil {
		for _, d := range hunterSeed() {
			hunterStore.defs[d.ID] = d
		}
		return
	}
	var list []hunterDef
	if json.Unmarshal(raw, &list) == nil {
		for _, d := range list {
			hunterStore.defs[d.ID] = d
		}
	}
}

var hunterSeedTier = regexp.MustCompile(`T5$`)

func hunterSeed() []hunterDef {
	c := bossCatalogData()
	var out []hunterDef
	for _, b := range c.Bases {
		if !b.AIPawnT || !hunterSeedTier.MatchString(b.ID) || b.ID != b.HullLine+"T5" {
			continue
		}
		cls, maker := "", baseShipManufacturerByClassSize[b.HullLine]
		for _, cl := range bossClasses {
			if strings.HasPrefix(b.HullLine, cl.ID) {
				cls = cl.ID
			}
		}
		name := b.ID
		if len(b.Hulls) > 0 {
			name = b.Hulls[0]
		}
		if len([]rune(name)) > 30 {
			name = string([]rune(name)[:30])
		}
		out = append(out, hunterDef{bossDef: bossDef{ID: len(out) + 1, Name: name, Tier: "legendary", Class: cls,
			Manufacturer: maker, Base: b.ID, Mult: 1, Modules: []string{}, Feats: []bossFeat{}}, Weight: 1})
	}
	return out
}

func hunterSaveLocked() error {
	raw, err := json.MarshalIndent(hunterListLocked(), "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(hunterFile()), 0o755); err != nil {
		return err
	}
	tmp := hunterFile() + ".tmp"
	if err := os.WriteFile(tmp, append(raw, '\n'), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, hunterFile())
}

func hunterListLocked() []hunterDef {
	list := make([]hunterDef, 0, len(hunterStore.defs))
	for _, d := range hunterStore.defs {
		list = append(list, d)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].ID < list[j].ID })
	return list
}

func hunterList() []hunterDef {
	hunterStore.Lock()
	defer hunterStore.Unlock()
	hunterLoadLocked()
	return hunterListLocked()
}

// hunterPick draws n hunters by weight, without repeats until every weighted
// hunter has been drawn once.
func hunterPick(n int) []hunterDef {
	var pool []hunterDef
	for _, d := range hunterList() {
		if d.Weight > 0 {
			pool = append(pool, d)
		}
	}
	var out []hunterDef
	left := append([]hunterDef(nil), pool...)
	for len(out) < n && len(pool) > 0 {
		if len(left) == 0 {
			left = append(left, pool...)
		}
		total := 0
		for _, d := range left {
			total += d.Weight
		}
		r := rand.Intn(total)
		for i, d := range left {
			if r -= d.Weight; r < 0 {
				out = append(out, d)
				left = append(left[:i], left[i+1:]...)
				break
			}
		}
	}
	return out
}

func (d *hunterDef) validate() error {
	if d.Weight < 0 || d.Weight > 100 {
		return fmt.Errorf("weight must be 0-100")
	}
	if d.Mult == 0 {
		d.Mult = 1
	}
	d.Marker = "" // hunters fly unmarked
	if err := d.bossDef.validate(); err != nil {
		return fmt.Errorf("%s", strings.Replace(err.Error(), "boss id", "hunter id", 1))
	}
	return nil
}

// ---- admin API ----------------------------------------------------------

func registerAdminHunters(api *mux.Router) {
	api.HandleFunc("/hunters", func(w http.ResponseWriter, _ *http.Request) {
		writeAdminJSON(w, map[string]any{"hunters": hunterList()})
	}).Methods(http.MethodGet)
	api.HandleFunc("/hunters/{id:[0-9]+}", adminAPIHunterPut).Methods(http.MethodPost, http.MethodPut)
	api.HandleFunc("/hunters/{id:[0-9]+}", adminAPIHunterDelete).Methods(http.MethodDelete)
	api.HandleFunc("/hunters/{id:[0-9]+}/spawn", func(w http.ResponseWriter, r *http.Request) {
		id, _ := strconv.Atoi(mux.Vars(r)["id"])
		hunterStore.Lock()
		hunterLoadLocked()
		d, ok := hunterStore.defs[id]
		hunterStore.Unlock()
		if !ok {
			adminBossError(w, http.StatusNotFound, "no such hunter")
			return
		}
		queueHostCommand("", fmt.Sprintf("hunterdef %d", id))
		writeAdminJSON(w, map[string]any{"sent": fmt.Sprintf("Hunter %d %s sent to every running match.", id, d.Name)})
	}).Methods(http.MethodPost)
}

func adminAPIHunterPut(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.Atoi(mux.Vars(r)["id"])
	var d hunterDef
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64<<10)).Decode(&d); err != nil {
		adminBossError(w, http.StatusBadRequest, "bad JSON: "+err.Error())
		return
	}
	hunterStore.Lock()
	defer hunterStore.Unlock()
	hunterLoadLocked()
	if id == 0 { // a new hunter: the server numbers it
		for k := range hunterStore.defs {
			id = max(id, k)
		}
		id++
	}
	d.ID = id
	if err := d.validate(); err != nil {
		adminBossError(w, http.StatusBadRequest, err.Error())
		return
	}
	from, _ := strconv.Atoi(r.URL.Query().Get("from"))
	if _, taken := hunterStore.defs[id]; taken && from != 0 && from != id {
		adminBossError(w, http.StatusConflict, fmt.Sprintf("hunter %d already exists", id))
		return
	}
	prev, had := hunterStore.defs[from]
	old, hadID := hunterStore.defs[id]
	hunterStore.defs[id] = d
	if from != 0 && from != id && had {
		delete(hunterStore.defs, from)
	}
	if err := hunterSaveLocked(); err != nil {
		delete(hunterStore.defs, id)
		if hadID {
			hunterStore.defs[id] = old
		}
		if had {
			hunterStore.defs[from] = prev
		}
		adminBossError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if db := currentMmogPlayerStateDB(); db != nil {
		adminAudit(db, r, "save hunter", strconv.Itoa(id), map[string]any{"name": d.Name, "base": d.Base, "weight": d.Weight})
	}
	writeAdminJSON(w, map[string]any{"hunters": hunterListLocked(), "id": id})
}

func adminAPIHunterDelete(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.Atoi(mux.Vars(r)["id"])
	hunterStore.Lock()
	defer hunterStore.Unlock()
	hunterLoadLocked()
	d, ok := hunterStore.defs[id]
	if !ok {
		adminBossError(w, http.StatusNotFound, "no such hunter")
		return
	}
	delete(hunterStore.defs, id)
	if err := hunterSaveLocked(); err != nil {
		hunterStore.defs[id] = d
		adminBossError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if db := currentMmogPlayerStateDB(); db != nil {
		adminAudit(db, r, "delete hunter", strconv.Itoa(id), map[string]any{"name": d.Name})
	}
	writeAdminJSON(w, map[string]any{"hunters": hunterListLocked()})
}

// ---- battle host --------------------------------------------------------

// battleHuntersHandler: GET /battle/hunters?n=K (weighted picks) or ?id=N
// (that one), loopback only. "ok=1\ncount=<k>\n", then per hunter a "--"
// line and the boss key=value block plus mult.
func battleHuntersHandler(w http.ResponseWriter, r *http.Request) {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		http.Error(w, "loopback only", http.StatusForbidden)
		return
	}
	var picks []hunterDef
	if id, _ := strconv.Atoi(r.URL.Query().Get("id")); id > 0 {
		hunterStore.Lock()
		hunterLoadLocked()
		if d, ok := hunterStore.defs[id]; ok {
			picks = append(picks, d)
		}
		hunterStore.Unlock()
	} else {
		n, _ := strconv.Atoi(r.URL.Query().Get("n"))
		picks = hunterPick(max(0, min(12, n)))
	}
	var b strings.Builder
	fmt.Fprintf(&b, "ok=1\ncount=%d\n", len(picks))
	for _, d := range picks {
		fmt.Fprintf(&b, "--\n%smult=%d\n", bossHostText(d.bossDef), d.Mult)
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = fmt.Fprint(w, b.String())
}
