package main

import (
	"bytes"
	"database/sql"
	_ "embed"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	dreadconfig "github.com/darkace1998/Dreadnought-Revival-project/shared/dreadgameconfig"
	"github.com/sirupsen/logrus"
)

// Fleet manager (operator 2026-10-22: "a list of all loadouts, and then the
// active ones which are separate and the ones in game, with ability to save as
// or replace"). The game keeps two loadouts per ship (LOADOUT A, the
// player_ship_loadouts row, and B, loadout_variants.go). The launcher's Fleet
// page adds a library of saved loadouts per player (player_saved_loadouts):
// save A or B into it, overwrite an entry with A or B, or write an entry back
// into A or B. Fitting itself stays in the game; the library only stores fits
// the player already made, so every item in it came from their own loadouts.
//
// Writing A or B is refused while the game is connected: the client loads a
// loadout's contents once per ID (InitializeFromPlayerData 0x34BD00 skips IDs
// it already has, loadout_variants.go) and its next YA_UpdateShipLoadout would
// write its stale copy back over ours.
//
// The page is the admin dashboard in fleet mode (DN_FLEET: the boss tree and
// editor over the player's own ships), served like the battle view: fetched
// through the launcher (/launcher/fleet/page) into a frame whose requests the
// launcher carries (dn-launcher skins_windows.go ViewerAPI, /fleet paths).

const fleetSavedMax = 200

type fleetFit struct {
	Weapons   [2]int32 `json:"weapons"`
	Abilities [4]int32 `json:"abilities"`
	Perks     [4]int32 `json:"perks"` // com, weapon, navigation, engineering
}

// fleetOption: an item the player owns and what it fits -- the boss
// catalog's class and slot, a main gun also its hull weight (H|M|L).
type fleetOption struct {
	ID    int32  `json:"id"`
	Kind  string `json:"kind"` // weapon|module|officer
	Slot  string `json:"slot"`
	Class string `json:"class,omitempty"`
	W     string `json:"w,omitempty"`
	Tier  int    `json:"tier,omitempty"`
}

func fleetOptions(c bossCatalog, uid string) []fleetOption {
	owned := ownedItemSet(uid)
	var out []fleetOption
	for _, g := range []struct {
		kind string
		list []bossCatalogItem
	}{{"weapon", c.Weapons}, {"module", c.Modules}, {"officer", c.Officers}} {
		for _, it := range g.list {
			if it.ID > 0 && owned[it.ID] {
				o := fleetOption{ID: it.ID, Kind: g.kind, Slot: it.Slot, Class: it.Class, Tier: it.Tier}
				if g.kind == "weapon" && it.Slot == "primary" {
					o.W = it.Note
				}
				out = append(out, o)
			}
		}
	}
	// Cosmetics: owned or a hull's default; a part also its slot and hull line
	// (a hull's default parts are named "None"; a "None" paint etc. is not an
	// item, the empty slot is).
	defaults := dreadconfig.AllDefaultShipVanityItemIDs()
	for _, g := range []struct {
		kind string
		list []bossCatalogItem
	}{{"paint", c.Paints}, {"pattern", c.Patterns}, {"decal", c.Decals}, {"emblem", c.Emblems}, {"part", c.Parts}} {
		for _, it := range g.list {
			if it.ID > 0 && (owned[it.ID] || defaults[it.ID]) && (g.kind == "part" || strings.TrimSpace(it.Name) != "None") {
				out = append(out, fleetOption{ID: it.ID, Kind: g.kind, Slot: it.Slot, Class: it.Class})
			}
		}
	}
	return out
}

var (
	fleetModSlots  = [4]string{"primary", "secondary", "perimeter", "internal"}
	fleetPerkSlots = [4]string{"com", "wpn", "nav", "eng"}
)

// fleetCheckFit checks an edited fit against the ship: each changed slot holds
// an owned item of that slot (guns and modules of the ship's class, a main gun
// of its weight), the main gun is never empty. It returns the fit as stored:
// unchanged slots keep their stored id, changed ones take the catalog's
// shared id (what the stored loadouts hold), an emptied one -1 (as stored
// officer slots are; 0 and -1 both read as empty).
func fleetCheckFit(c bossCatalog, uid string, l mmogShipLoadoutSeed, cur, f fleetFit) (fleetFit, error) {
	hull, _, class, _, _ := fleetShipLook(l)
	weight := ""
	for _, w := range []string{"Heavy", "Medium", "Light"} {
		if strings.HasSuffix(hull, w) {
			weight = w[:1]
		}
	}
	opts := map[int32]fleetOption{}
	for _, o := range fleetOptions(c, uid) {
		opts[o.ID] = o
	}
	slot := func(id, was int32, kind, sl string, mainGun bool) (int32, error) {
		if id == was || (id > 0 && fleetSharedID(c, id) == fleetSharedID(c, was)) {
			return was, nil
		}
		if id <= 0 {
			if mainGun {
				return 0, fmt.Errorf("the main gun cannot be empty")
			}
			if was <= 0 {
				return was, nil
			}
			return -1, nil
		}
		o, ok := opts[fleetSharedID(c, id)]
		if !ok || o.Kind != kind || o.Slot != sl || (kind != "officer" && o.Class != class) || (mainGun && o.W != weight) {
			return 0, fmt.Errorf("that item does not fit this slot")
		}
		return o.ID, nil
	}
	var out fleetFit
	var err error
	for i, sl := range []string{"primary", "secondary"} {
		if out.Weapons[i], err = slot(f.Weapons[i], cur.Weapons[i], "weapon", sl, i == 0); err != nil {
			return out, err
		}
	}
	for i, sl := range fleetModSlots {
		if out.Abilities[i], err = slot(f.Abilities[i], cur.Abilities[i], "module", sl, false); err != nil {
			return out, err
		}
	}
	for i, sl := range fleetPerkSlots {
		if out.Perks[i], err = slot(f.Perks[i], cur.Perks[i], "officer", sl, false); err != nil {
			return out, err
		}
	}
	return out, nil
}

// fleetCheckLook checks an edited appearance and returns the display info to
// store. Unchanged slots keep their stored text; a changed one must be an owned
// cosmetic of that kind (a part of that slot and the ship's hull line) and is
// written per-ship like the client writes it -- the shared id with the class
// byte the stored string already carries (sharedGearID). Parts are never
// empty (the importer wants four); paint, pattern, decal and emblem may be
// (-1, VanityUnsetSlot). A hero cannot be customised (displayInfo).
func fleetCheckLook(c bossCatalog, uid string, l mmogShipLoadoutSeed, k fleetLook) (string, bool, error) {
	info := l.displayInfo()
	cur := lookOf(info)
	if k == cur {
		return info, false, nil
	}
	if _, hero := heroByID(l.precastLoadoutID); hero {
		return "", false, fmt.Errorf("a hero ship cannot be customised")
	}
	g := strings.Split(info, ";")
	for len(g) < 5 {
		g = append(g, dreadconfig.VanityUnsetSlot)
	}
	mesh := strings.Split(g[0], "#")
	for len(mesh) < 4 {
		mesh = append(mesh, dreadconfig.VanityUnsetSlot)
	}
	cls := int32(-1)
	for _, f := range append(append([]string{}, mesh...), g[1:]...) {
		if v, err := strconv.ParseInt(strings.TrimSpace(f), 10, 32); err == nil && v > 0 {
			if b := int32(v>>16) & 0xff; b != 0xff && (v>>24)&0xff >= 20 && (v>>24)&0xff <= 24 {
				cls = b
				break
			}
		}
	}
	hull, _, _, _, _ := fleetShipLook(l)
	opts := map[int32]fleetOption{}
	for _, o := range fleetOptions(c, uid) {
		opts[o.ID] = o
	}
	slot := func(id, was int32, kind string, field *string, part bool) error {
		if id == was {
			return nil
		}
		if id <= 0 {
			if part {
				return fmt.Errorf("a ship part cannot be empty")
			}
			*field = dreadconfig.VanityUnsetSlot
			return nil
		}
		o, ok := opts[id]
		if !ok || o.Kind != kind || (part && o.Class != hull) {
			return fmt.Errorf("that cosmetic does not fit this slot")
		}
		if cls < 0 {
			return fmt.Errorf("this ship's appearance cannot be edited")
		}
		*field = strconv.Itoa(int(id&^0x00FF0000 | cls<<16))
		return nil
	}
	seen := map[string]bool{}
	for i := range k.Parts {
		if err := slot(k.Parts[i], cur.Parts[i], "part", &mesh[i], true); err != nil {
			return "", false, err
		}
		if o, ok := opts[k.Parts[i]]; ok && k.Parts[i] != cur.Parts[i] {
			if seen[o.Slot] {
				return "", false, fmt.Errorf("that cosmetic does not fit this slot")
			}
			seen[o.Slot] = true
		}
	}
	for i, x := range []struct {
		id, was int32
		kind    string
	}{{k.Emblem, cur.Emblem, "emblem"}, {k.Paint, cur.Paint, "paint"}, {k.Pattern, cur.Pattern, "pattern"}, {k.Decal, cur.Decal, "decal"}} {
		if err := slot(x.id, x.was, x.kind, &g[i+1], false); err != nil {
			return "", false, err
		}
	}
	g[0] = strings.Join(mesh[:4], "#")
	return strings.Join(g[:5], ";"), true, nil
}

func fitOf(l mmogShipLoadoutSeed) fleetFit {
	return fleetFit{Weapons: [2]int32{l.weaponPrimaryID, l.weaponSecondaryID}, Abilities: l.abilityIDs, Perks: l.perkIDs}
}

type fleetLoadoutView struct {
	Variant int32     `json:"variant"` // 0 = A, 1 = B
	Name    string    `json:"name"`
	Fit     fleetFit  `json:"fit"`
	Look    fleetLook `json:"look"`
}

// fleetLook: the ship's appearance (its display info, "p#p#p#p;emblem;paint;
// pattern;decal"), as the catalog's shared item ids; 0 = empty.
type fleetLook struct {
	Parts   [4]int32 `json:"parts"`
	Emblem  int32    `json:"emblem"`
	Paint   int32    `json:"paint"`
	Pattern int32    `json:"pattern"`
	Decal   int32    `json:"decal"`
}

func lookOf(info string) fleetLook {
	var k fleetLook
	id := func(f string) int32 {
		v, err := strconv.ParseInt(strings.TrimSpace(f), 10, 32)
		if err != nil || v <= 0 {
			return 0
		}
		return sharedGearID(int32(v))
	}
	g := strings.Split(info, ";")
	for len(g) < 5 {
		g = append(g, "")
	}
	for i, f := range strings.Split(g[0], "#") {
		if i < 4 {
			k.Parts[i] = id(f)
		}
	}
	k.Emblem, k.Paint, k.Pattern, k.Decal = id(g[1]), id(g[2]), id(g[3]), id(g[4])
	return k
}

func (k fleetLook) ids() []int32 {
	return append(k.Parts[:], k.Emblem, k.Paint, k.Pattern, k.Decal)
}

func loadoutView(l mmogShipLoadoutSeed) fleetLoadoutView {
	return fleetLoadoutView{Variant: l.variant, Name: l.loadoutName, Fit: fitOf(l), Look: lookOf(l.displayInfo())}
}

type fleetShipView struct {
	ID       int32              `json:"id"` // loadout id (the ship's LOADOUT A row)
	Name     string             `json:"name"`
	HullLine string             `json:"hull_line"`
	Class    string             `json:"class"`
	Maker    string             `json:"maker"`
	Tier     int32              `json:"tier"`
	Hero     bool               `json:"hero,omitempty"`
	Img      string             `json:"img,omitempty"`      // key into "art" (ships share pictures)
	HullImg  string             `json:"hull_img,omitempty"` // the hull's picture (a hero's Img is its own)
	Loadouts []fleetLoadoutView `json:"loadouts"`
}

type fleetSaved struct {
	ID      int64    `json:"id"`
	Name    string   `json:"name"`
	Ship    int32    `json:"ship"`
	Fit     fleetFit `json:"fit"`
	Created string   `json:"created"`
}

type fleetItemView struct {
	Name  string      `json:"name"`
	Kind  string      `json:"kind"`           // weapon|module|officer
	Icon  string      `json:"icon,omitempty"` // art key: GET /launcher/fleet/art
	Desc  string      `json:"desc,omitempty"`
	Tips  [][2]string `json:"tips,omitempty"`
	Stats [][2]string `json:"stats,omitempty"`
	// Cosmetics: kind paint|pattern|decal|emblem|part; a paint's colours, a part's slot.
	Swatch []string `json:"swatch,omitempty"`
	Slot   string   `json:"slot,omitempty"`
}

// fleetVanityItem: a cosmetic by its shared id, pictured like the boss
// editor's (bVanPic): the market icon unless generic, else the decoded texture.
func fleetVanityItem(c bossCatalog, id int32) (fleetItemView, bool) {
	v := fleetItemView{}
	for _, g := range []struct {
		kind string
		list []bossCatalogItem
	}{{"paint", c.Paints}, {"pattern", c.Patterns}, {"decal", c.Decals}, {"emblem", c.Emblems}, {"part", c.Parts}} {
		for _, it := range g.list {
			if it.ID == id {
				v.Kind, v.Name, v.Slot = g.kind, strings.TrimSpace(it.Name), it.Slot
			}
		}
	}
	if v.Kind == "" {
		return v, false
	}
	key := strconv.Itoa(int(id))
	str := func(kind string) string {
		var x string
		if raw, ok := fleetArtTable(kind)[key]; ok {
			_ = json.Unmarshal(raw, &x)
		}
		return x
	}
	if f := str("vanity"); f != "" && !strings.HasPrefix(f, "UI_Vanity_") {
		v.Icon = "vanity/" + f
	} else if p := str("previews"); p != "" {
		v.Icon = p
	} else if f != "" {
		v.Icon = "vanity/" + f
	}
	if raw, ok := fleetArtTable("paints")[key]; ok {
		_ = json.Unmarshal(raw, &v.Swatch)
	}
	return v, true
}

// ---- art (data/admin-art, the dashboard's), as data: URIs ----------------

var fleetArt = struct {
	sync.Mutex
	at   time.Time
	art  map[string]map[string]json.RawMessage
	uris map[string]string
}{}

func fleetArtTable(kind string) map[string]json.RawMessage {
	fleetArt.Lock()
	defer fleetArt.Unlock()
	file := filepath.Join(dreadconfig.DataDir(), "admin-art", "art.json")
	if st, err := os.Stat(file); err == nil && (fleetArt.art == nil || st.ModTime().After(fleetArt.at)) {
		var a map[string]map[string]json.RawMessage
		if raw, err := os.ReadFile(file); err == nil && json.Unmarshal(raw, &a) == nil {
			fleetArt.art, fleetArt.at, fleetArt.uris = a, st.ModTime(), map[string]string{}
		}
	}
	return fleetArt.art[kind]
}

func fleetArtURI(rel string) string {
	if rel == "" || strings.Contains(rel, "..") {
		return ""
	}
	fleetArt.Lock()
	defer fleetArt.Unlock()
	if fleetArt.uris == nil {
		fleetArt.uris = map[string]string{}
	}
	if u, ok := fleetArt.uris[rel]; ok {
		return u
	}
	u := ""
	// As handleAdminArt: vanity/<name> is a decoded cosmetic picture in the market images.
	file := filepath.Join(dreadconfig.DataDir(), "admin-art", filepath.FromSlash(rel))
	if rest, ok := strings.CutPrefix(rel, "vanity/"); ok {
		file = filepath.Join(dreadconfig.MarketImagesDir(), filepath.FromSlash(rest))
	}
	if b, err := os.ReadFile(file); err == nil {
		u = "data:" + http.DetectContentType(b) + ";base64," + base64.StdEncoding.EncodeToString(b)
	}
	fleetArt.uris[rel] = u
	return u
}

// fleetItem describes one item id from the boss catalog (item id -> asset)
// and the dashboard art (asset -> name, icon, tooltip rows).
func fleetItem(c bossCatalog, id int32) (fleetItemView, bool) {
	var a struct {
		Name  string            `json:"name"`
		Icon  string            `json:"icon"`
		Desc  string            `json:"desc"`
		Tips  [][2]string       `json:"tips"`
		Stats map[string]string `json:"stats"`
	}
	look := func(kind, key string) {
		if raw, ok := fleetArtTable(kind)[key]; ok {
			_ = json.Unmarshal(raw, &a)
		}
	}
	v := fleetItemView{}
	if id = fleetSharedID(c, id); id <= 0 {
		return v, false
	}
	for _, m := range c.Modules {
		if m.ID == id {
			v.Kind, v.Name = "module", m.Name
			look("modules", m.Path)
			break
		}
	}
	if v.Kind == "" {
		for _, o := range c.Officers {
			if o.ID == id {
				v.Kind, v.Name = "officer", o.Name
				look("officers", o.Path)
				break
			}
		}
	}
	if v.Kind == "" {
		for _, w := range c.Weapons {
			if w.ID == id {
				v.Kind, v.Name = "weapon", w.Name
				look("weapons", w.Path)
				break
			}
		}
	}
	if v.Kind == "" {
		return v, false
	}
	if a.Name != "" {
		v.Name = a.Name
	}
	if n, ok := authoritativeItemName(id); ok && n != "" {
		v.Name = n
	}
	v.Icon, v.Desc, v.Tips = a.Icon, a.Desc, a.Tips
	keys := make([]string, 0, len(a.Stats))
	for k := range a.Stats {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if a.Stats[k] != "" {
			v.Stats = append(v.Stats, [2]string{k, a.Stats[k]})
		}
	}
	return v, true
}

// fleetSharedID: an item id as the boss catalog keys it. Loadouts can hold a
// weapon's per-ship ("inflated", id_mirror.go) id -- e.g. 84803596, Athos's
// copy (ItemIDConversionTable InflatedItemIDs) -- where the catalog has the
// shared one.
func fleetSharedID(c bossCatalog, id int32) int32 {
	if id <= 0 {
		return 0
	}
	for _, list := range [][]bossCatalogItem{c.Modules, c.Officers, c.Weapons} {
		for _, it := range list {
			if it.ID == id {
				return id
			}
		}
	}
	return deInflatedItemID(id)
}

// fleetShipLook: hull line, tier, class, maker of a player loadout.
func fleetShipLook(l mmogShipLoadoutSeed) (hull string, tier int32, class, maker string, hero bool) {
	if h, ok := heroByID(l.precastLoadoutID); ok {
		hull, tier, maker, hero = h.hullLine, h.tier, h.manufacturer, true
	} else {
		for _, b := range baseShipLoadouts {
			if b.loadoutID == l.precastLoadoutID {
				hull, tier = b.hullLine, b.tier
			}
		}
		if hull == "" {
			hull, _ = shipHullLine(l.ship.id)
		}
		maker = baseShipManufacturerByClassSize[hull]
		// Not a base precast (an upgraded or variant ship): tier 0 made /clone
		// pick the T1 AI hull for a T5 ship (operator 2026-10-24).
		if tier < 1 {
			if t, ok := shipTierForIDChecked(l.ship.id); ok {
				tier = t
			} else if t, ok := shipTierForIDChecked(l.precastLoadoutID); ok {
				tier = t
			}
		}
	}
	for _, cl := range bossClasses {
		if strings.HasPrefix(hull, cl.ID) {
			class = cl.ID
		}
	}
	return
}

// ---- store ---------------------------------------------------------------

func fleetSavedList(db *sql.DB, uid string) ([]fleetSaved, error) {
	rows, err := db.Query(`SELECT id, name, ship_loadout_id, weapon_primary_id, weapon_secondary_id,
		ability_primary_id, ability_secondary_id, ability_perimeter_id, ability_internal_id,
		perk_com_id, perk_weapon_id, perk_navigation_id, perk_engineer_id, created_at
		FROM player_saved_loadouts WHERE user_id=? ORDER BY id`, uid)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()
	out := []fleetSaved{}
	for rows.Next() {
		var s fleetSaved
		f := &s.Fit
		if err := rows.Scan(&s.ID, &s.Name, &s.Ship, &f.Weapons[0], &f.Weapons[1], &f.Abilities[0], &f.Abilities[1], &f.Abilities[2], &f.Abilities[3],
			&f.Perks[0], &f.Perks[1], &f.Perks[2], &f.Perks[3], &s.Created); err != nil {
			return nil, err
		}
		out = append(out, s)
	}
	return out, rows.Err()
}

func fitArgs(f fleetFit) []any {
	return []any{f.Weapons[0], f.Weapons[1], f.Abilities[0], f.Abilities[1], f.Abilities[2], f.Abilities[3], f.Perks[0], f.Perks[1], f.Perks[2], f.Perks[3]}
}

// fleetPlayerLoadouts: the player's ships with A (and B where the ship has it).
func fleetPlayerLoadouts(uid string) []mmogShipLoadoutSeed {
	return withLoadoutVariants(uid, ownedShipLoadoutsForPlayerData(mmogPlayerStateForPID(uid), uid))
}

func fleetFindLoadout(uid string, ship, variant int32) (mmogShipLoadoutSeed, bool) {
	for _, l := range fleetPlayerLoadouts(uid) {
		if l.loadoutID() == ship && l.variant == variant {
			return l, true
		}
	}
	return mmogShipLoadoutSeed{}, false
}

func fleetGameOnline(uid string) bool {
	for _, id := range socialHubInstance.onlinePlayerIDs() {
		if normalizedPlayerStatePID(id) == uid {
			return true
		}
	}
	return false
}

// ---- handlers --------------------------------------------------------------

func launcherFleetError(w http.ResponseWriter, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func launcherFleetPage(w http.ResponseWriter, r *http.Request) {
	if launcherUID(r) == "" {
		http.Error(w, "unavailable", http.StatusNotFound)
		return
	}
	page := adminDashboardHTML
	if p := os.Getenv("DN_FLEET_HTML"); p != "" {
		if b, err := os.ReadFile(p); err == nil {
			page = b
		}
	}
	// The ship preview's three.js stays: the launcher points its /admin/art/
	// URLs at its loopback art proxy (window.DN_ART); without one the
	// imports fail and the Cosmetics tab shows the ship's picture.
	c := bossCatalogData()
	// "<hull line>T<tier>" -> the hull's name, the plain "<line>T<n>" base first.
	hulls := map[string]string{}
	for _, b := range c.Bases {
		k := fmt.Sprintf("%sT%d", b.HullLine, b.Tier)
		if len(b.Hulls) > 0 && (hulls[k] == "" || b.ID == k) {
			hulls[k] = b.Hulls[0]
		}
	}
	cfg, _ := json.Marshal(map[string]any{
		"cat": map[string]any{"tiers": c.Tiers, "classes": c.Classes, "manufacturers": c.Manufacturers, "hulls": hulls},
		"ui":  fleetArtTable("ui"), // badge, logo and tier pictures: art keys like the ships'
	})
	page = bytes.Replace(page, []byte("<script>"), []byte("<script>window.DN_FLEET = "+string(cfg)+";</script>\n<script>"), 1)
	// Dark from the first paint: the html.fleet class (and its color-scheme) comes
	// only when the script runs, and until then WebView2 paints the frame white.
	page = bytes.Replace(page, []byte("<head>"), []byte(`<head><meta name="color-scheme" content="dark"><style>html{background:transparent}</style>`), 1)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write(page)
}

// launcherFleetHandler: GET /launcher/fleet -- ships (A/B), active fleets,
// the library, and every item they name.
func launcherFleetHandler(w http.ResponseWriter, r *http.Request) {
	uid := launcherUID(r)
	db := currentMmogPlayerStateDB()
	if uid == "" || db == nil {
		launcherFleetError(w, http.StatusServiceUnavailable, "unavailable")
		return
	}
	c := bossCatalogData()
	ids := map[int32]bool{}
	note := func(f fleetFit) {
		for _, v := range append(append(f.Weapons[:], f.Abilities[:]...), f.Perks[:]...) {
			if v > 0 {
				ids[v] = true
			}
		}
	}
	var ships []*fleetShipView
	byID := map[int32]*fleetShipView{}
	looks := map[int32]bool{}
	for _, l := range fleetPlayerLoadouts(uid) {
		s := byID[l.loadoutID()]
		if s == nil {
			hull, tier, class, maker, hero := fleetShipLook(l)
			s = &fleetShipView{ID: l.loadoutID(), Name: l.loadoutName, HullLine: hull, Class: class, Maker: maker, Tier: tier, Hero: hero}
			if len(hull) > len(class) && tier > 0 {
				key := fmt.Sprintf("ships/%s%s_T%d.png", class, hull[len(class):len(class)+1], tier)
				if fleetArtURI(key) != "" {
					s.Img, s.HullImg = key, key
				}
			}
			// A hero ship's own picture: its Market icon (data/market-images).
			if m, ok := dreadconfig.HeroMarketData(s.ID); hero && ok && m.Image != "" && fleetArtURI("vanity/"+m.Image) != "" {
				s.Img = "vanity/" + m.Image
			}
			byID[s.ID] = s
			ships = append(ships, s)
		}
		lv := loadoutView(l)
		note(lv.Fit)
		for _, id := range lv.Look.ids() {
			looks[id] = true
		}
		s.Loadouts = append(s.Loadouts, lv)
	}
	saved, err := fleetSavedList(db, uid)
	if err != nil {
		launcherFleetError(w, http.StatusInternalServerError, err.Error())
		return
	}
	for _, s := range saved {
		note(s.Fit)
	}
	state := mmogPlayerStateForPID(uid)
	type fleetGroup struct {
		Name   string  `json:"name"`
		Active bool    `json:"active"`
		Ships  []int32 `json:"ships"`
		Flag   int32   `json:"flagship"`
	}
	fleets := []fleetGroup{}
	for _, f := range state.fleets {
		g := fleetGroup{Name: f.displayName, Active: f.active, Ships: []int32{}, Flag: f.flagshipLoadoutID}
		for _, l := range f.shipLoadouts {
			g.Ships = append(g.Ships, l.loadoutID())
		}
		fleets = append(fleets, g)
	}
	items := map[string]fleetItemView{}
	for id := range ids {
		if v, ok := fleetItem(c, id); ok {
			items[strconv.Itoa(int(id))] = v
		}
	}
	for id := range looks {
		if v, ok := fleetVanityItem(c, id); ok {
			items[strconv.Itoa(int(id))] = v
		}
	}
	opts := fleetOptions(c, uid)
	for _, o := range opts {
		if v, ok := fleetItem(c, o.ID); ok {
			items[strconv.Itoa(int(o.ID))] = v
		} else if v, ok := fleetVanityItem(c, o.ID); ok {
			items[strconv.Itoa(int(o.ID))] = v
		}
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"ships": ships, "fleets": fleets, "saved": saved, "items": items,
		"online": fleetGameOnline(uid), "max": fleetSavedMax, "opts": opts})
}

// launcherFleetArt: GET /launcher/fleet/art?k=<key>,<key>,... -> {key: data URI}.
// The pictures are most of the page's weight (~4 MB); sent with the fleet
// data they took longer than the launcher bridge's timeout under Wine, so the
// page asks for them in small batches after it has drawn.
func launcherFleetArt(w http.ResponseWriter, r *http.Request) {
	out := map[string]string{}
	for i, k := range strings.Split(r.URL.Query().Get("k"), ",") {
		if i >= 40 {
			break
		}
		if k = strings.TrimSpace(k); k != "" {
			if u := fleetArtURI(k); u != "" {
				out[k] = u
			}
		}
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(out)
}

// launcherFleetAction: POST /launcher/fleet/action
//
//	{"op":"save",    "ship":S, "variant":V, "name":N}         A/B -> new library entry
//	{"op":"replace", "id":I,   "ship":S, "variant":V}         A/B -> over entry I
//	{"op":"apply",   "id":I,   "variant":V}                   entry I -> A/B of its ship
//	{"op":"set",     "ship":S, "variant":V, "fit":F, "look":K} edited fit (and look) -> A/B (game closed)
//	{"op":"edit",    "id":I,   "name":N, "fit":F}             entry I name + fit
//	{"op":"rename",  "id":I,   "name":N}
//	{"op":"delete",  "id":I}
func launcherFleetAction(w http.ResponseWriter, r *http.Request) {
	uid := launcherUID(r)
	db := currentMmogPlayerStateDB()
	if uid == "" || db == nil {
		launcherFleetError(w, http.StatusServiceUnavailable, "unavailable")
		return
	}
	var req struct {
		Op      string     `json:"op"`
		ID      int64      `json:"id"`
		Ship    int32      `json:"ship"`
		Variant int32      `json:"variant"`
		Name    string     `json:"name"`
		Fit     fleetFit   `json:"fit"`
		Look    *fleetLook `json:"look"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4<<10)).Decode(&req); err != nil {
		launcherFleetError(w, http.StatusBadRequest, "bad request")
		return
	}
	req.Name = strings.TrimSpace(req.Name)
	if len([]rune(req.Name)) > 32 || strings.ContainsAny(req.Name, "\r\n") {
		launcherFleetError(w, http.StatusBadRequest, "name: up to 32 characters")
		return
	}
	entry := func() (fleetSaved, bool) {
		list, _ := fleetSavedList(db, uid)
		for _, s := range list {
			if s.ID == req.ID {
				return s, true
			}
		}
		return fleetSaved{}, false
	}
	var err error
	var appliedShip int32
	switch req.Op {
	case "save", "replace":
		l, ok := fleetFindLoadout(uid, req.Ship, req.Variant)
		if !ok {
			launcherFleetError(w, http.StatusNotFound, "no such loadout")
			return
		}
		if req.Op == "save" {
			var n int
			_ = db.QueryRow(`SELECT COUNT(*) FROM player_saved_loadouts WHERE user_id=?`, uid).Scan(&n)
			if n >= fleetSavedMax {
				launcherFleetError(w, http.StatusConflict, fmt.Sprintf("library full (%d)", fleetSavedMax))
				return
			}
			if req.Name == "" {
				req.Name = l.loadoutName + " " + string(rune('A'+req.Variant))
			}
			_, err = db.Exec(`INSERT INTO player_saved_loadouts (user_id, name, ship_loadout_id, weapon_primary_id, weapon_secondary_id,
				ability_primary_id, ability_secondary_id, ability_perimeter_id, ability_internal_id,
				perk_com_id, perk_weapon_id, perk_navigation_id, perk_engineer_id) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)`,
				append([]any{uid, req.Name, l.loadoutID()}, fitArgs(fitOf(l))...)...)
		} else {
			s, ok := entry()
			if !ok {
				launcherFleetError(w, http.StatusNotFound, "no such saved loadout")
				return
			}
			if s.Ship != l.loadoutID() {
				launcherFleetError(w, http.StatusBadRequest, "that saved loadout is for another ship")
				return
			}
			_, err = db.Exec(`UPDATE player_saved_loadouts SET weapon_primary_id=?, weapon_secondary_id=?,
				ability_primary_id=?, ability_secondary_id=?, ability_perimeter_id=?, ability_internal_id=?,
				perk_com_id=?, perk_weapon_id=?, perk_navigation_id=?, perk_engineer_id=?, created_at=datetime('now')
				WHERE user_id=? AND id=?`, append(fitArgs(fitOf(l)), uid, req.ID)...)
		}
	case "apply":
		s, ok := entry()
		if !ok {
			launcherFleetError(w, http.StatusNotFound, "no such saved loadout")
			return
		}
		if fleetGameOnline(uid) {
			launcherFleetError(w, http.StatusConflict, "close the game first")
			return
		}
		if _, ok := fleetFindLoadout(uid, s.Ship, req.Variant); !ok {
			launcherFleetError(w, http.StatusNotFound, "no such loadout on that ship")
			return
		}
		err, appliedShip = fleetWriteLoadout(db, uid, s.Ship, req.Variant, s.Fit, ""), s.Ship
	case "set": // an edited fit into LOADOUT A/B
		l, ok := fleetFindLoadout(uid, req.Ship, req.Variant)
		if !ok {
			launcherFleetError(w, http.StatusNotFound, "no such loadout")
			return
		}
		if fleetGameOnline(uid) {
			launcherFleetError(w, http.StatusConflict, "close the game first")
			return
		}
		f, ferr := fleetCheckFit(bossCatalogData(), uid, l, fitOf(l), req.Fit)
		if ferr != nil {
			launcherFleetError(w, http.StatusBadRequest, ferr.Error())
			return
		}
		info := ""
		if req.Look != nil {
			in, changed, lerr := fleetCheckLook(bossCatalogData(), uid, l, *req.Look)
			if lerr != nil {
				launcherFleetError(w, http.StatusBadRequest, lerr.Error())
				return
			}
			if changed {
				info = in
			}
		}
		err, appliedShip = fleetWriteLoadout(db, uid, req.Ship, req.Variant, f, info), req.Ship
	case "edit": // a saved loadout's name and fit
		s, ok := entry()
		if !ok {
			launcherFleetError(w, http.StatusNotFound, "no such saved loadout")
			return
		}
		l, ok := fleetFindLoadout(uid, s.Ship, 0)
		if !ok {
			launcherFleetError(w, http.StatusNotFound, "no such ship")
			return
		}
		f, ferr := fleetCheckFit(bossCatalogData(), uid, l, s.Fit, req.Fit)
		if ferr != nil {
			launcherFleetError(w, http.StatusBadRequest, ferr.Error())
			return
		}
		if req.Name == "" {
			req.Name = s.Name
		}
		_, err = db.Exec(`UPDATE player_saved_loadouts SET name=?, weapon_primary_id=?, weapon_secondary_id=?,
			ability_primary_id=?, ability_secondary_id=?, ability_perimeter_id=?, ability_internal_id=?,
			perk_com_id=?, perk_weapon_id=?, perk_navigation_id=?, perk_engineer_id=? WHERE user_id=? AND id=?`,
			append(append([]any{req.Name}, fitArgs(f)...), uid, req.ID)...)
	case "rename":
		if req.Name == "" {
			launcherFleetError(w, http.StatusBadRequest, "name required")
			return
		}
		_, err = db.Exec(`UPDATE player_saved_loadouts SET name=? WHERE user_id=? AND id=?`, req.Name, uid, req.ID)
	case "delete":
		_, err = db.Exec(`DELETE FROM player_saved_loadouts WHERE user_id=? AND id=?`, uid, req.ID)
	default:
		launcherFleetError(w, http.StatusBadRequest, "unknown op")
		return
	}
	if err != nil {
		logrus.WithError(err).WithField("player", uid).Warn("fleet: " + req.Op)
		launcherFleetError(w, http.StatusInternalServerError, err.Error())
		return
	}
	// The reply is the library alone (with the items it names): the ships and
	// their pictures are unchanged, and the whole page runs to megabytes.
	saved, err := fleetSavedList(db, uid)
	if err != nil {
		launcherFleetError(w, http.StatusInternalServerError, err.Error())
		return
	}
	reply := map[string]any{"saved": saved, "online": fleetGameOnline(uid)}
	items := map[string]fleetItemView{}
	c := bossCatalogData()
	addItems := func(f fleetFit) {
		for _, v := range append(append(f.Weapons[:], f.Abilities[:]...), f.Perks[:]...) {
			if it, ok := fleetItem(c, v); ok {
				items[strconv.Itoa(int(v))] = it
			}
		}
	}
	for _, s := range saved {
		addItems(s.Fit)
	}
	if appliedShip != 0 { // that ship's loadouts changed: send them again
		lds := []fleetLoadoutView{}
		for _, l := range fleetPlayerLoadouts(uid) {
			if l.loadoutID() == appliedShip {
				lv := loadoutView(l)
				lds = append(lds, lv)
				addItems(lv.Fit)
				for _, id := range lv.Look.ids() {
					if it, ok := fleetVanityItem(c, id); ok {
						items[strconv.Itoa(int(id))] = it
					}
				}
			}
		}
		reply["ship"], reply["loadouts"] = appliedShip, lds
	}
	reply["items"] = items
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(reply)
}

// fleetWriteLoadout writes a fit into the ship's LOADOUT A (variant 0) or B
// (1, player_ship_loadout_variants slot 2, created from A first like a B save),
// and a non-empty info as its appearance.
func fleetWriteLoadout(db *sql.DB, uid string, ship, variant int32, f fleetFit, info string) error {
	set := `weapon_primary_id=?, weapon_secondary_id=?, ability_primary_id=?, ability_secondary_id=?, ability_perimeter_id=?,
		ability_internal_id=?, perk_com_id=?, perk_weapon_id=?, perk_navigation_id=?, perk_engineer_id=?,
		display_info=CASE WHEN ?='' THEN display_info ELSE ? END, updated_at=datetime('now')`
	args := func(tail ...any) []any { return append(append(fitArgs(f), info, info), tail...) }
	if variant == 0 {
		res, err := db.Exec(`UPDATE player_ship_loadouts SET `+set+` WHERE user_id=? AND loadout_id=?`, args(uid, ship)...)
		if err == nil {
			if n, _ := res.RowsAffected(); n == 0 {
				err = fmt.Errorf("ship %d has no loadout A", ship)
			}
		}
		return err
	}
	if ok, err := ensureLoadoutVariantRow(db, uid, ship, 2); err != nil || !ok {
		if err == nil {
			err = fmt.Errorf("ship %d has no loadout A", ship)
		}
		return err
	}
	_, err := db.Exec(`UPDATE player_ship_loadout_variants SET `+set+` WHERE user_id=? AND loadout_id=? AND slot=2`, args(uid, ship)...)
	return err
}

// ---- /clone: an AI copy of a player's ship as a custom boss ---------------

// Clones live in memory only, numbered from cloneIDBase so they never meet a
// saved boss (1-999); the host fetches them by id like any /boss
// (GET /battle/boss?id=N, bossByID).
const cloneIDBase = 10000

var cloneStore = struct {
	sync.Mutex
	next int
	defs map[int]bossDef
}{next: cloneIDBase, defs: map[int]bossDef{}}

func cloneByID(id int) (bossDef, bool) {
	cloneStore.Lock()
	defer cloneStore.Unlock()
	d, ok := cloneStore.defs[id]
	return d, ok
}

// cloneBossDef builds "Evil <name>" from a player's loadout: the Havoc AI base
// of the same hull line nearest its tier (a VH_*_AI_Pawn_T<n> must exist), the
// loadout's modules, officers and guns by item id through the boss catalog, and
// its coating, pattern, decal and emblem from the saved appearance string.
// Its hull parts are carried too (operator 2026-10-24: "why not?"); the host
// imports them into the AI hull. GUESS: player parts of the same hull line fit
// the AI hull's mesh slots -- the import is accepted, not seen on a client.
func cloneBossDef(name string, l mmogShipLoadoutSeed) (bossDef, error) {
	c := bossCatalogData()
	hull, tier, class, maker, _ := fleetShipLook(l)
	var base *bossCatalogBase
	for i := range c.Bases {
		b := &c.Bases[i]
		if b.HullLine != hull || !b.AIPawnT {
			continue
		}
		if base == nil || abs(b.Tier-int(tier)) < abs(base.Tier-int(tier)) {
			base = b
		}
	}
	if base == nil {
		return bossDef{}, fmt.Errorf("no AI ship of the %s line", hull)
	}
	evil := "Evil " + name
	if r := []rune(evil); len(r) > 30 {
		evil = string(r[:30])
	}
	d := bossDef{Name: evil, Class: class, Manufacturer: maker, Base: base.ID, Mult: 1, Feats: []bossFeat{}}
	for _, g := range bossTierGroups {
		if d.Tier == "" && bossTierAllows(g.ID, base.Tier) {
			d.Tier = g.ID
		}
	}
	byID := func(list []bossCatalogItem, id int32) string {
		id = fleetSharedID(c, id)
		for _, it := range list {
			if id > 0 && it.ID == id {
				return it.Path
			}
		}
		return ""
	}
	for i := 0; i < 4; i++ {
		d.Modules = append(d.Modules, byID(c.Modules, l.abilityIDs[i]))
		if p := byID(c.Officers, l.perkIDs[i]); p != "" {
			d.Officers = append(d.Officers, p)
		}
	}
	for len(d.Modules) > 0 && d.Modules[len(d.Modules)-1] == "" {
		d.Modules = d.Modules[:len(d.Modules)-1]
	}
	d.Weapons = []string{byID(c.Weapons, l.weaponPrimaryID), byID(c.Weapons, l.weaponSecondaryID)}
	// Appearance: "<parts>;<item>;<item>..." -- the category byte of each item
	// (Category law) says which it is: 21 emblem, 22 coating, 23 pattern, 24 decal.
	// The first group is the hull parts "fore#hull#bridge#stern" (category 20),
	// imported into the AI hull like a boss's parts (PaintPawn).
	groups := strings.Split(l.displayInfo(), ";")
	if len(groups) > 0 && strings.Contains(groups[0], "#") {
		for i, f := range strings.Split(groups[0], "#") {
			if v, err := strconv.ParseInt(strings.TrimSpace(f), 10, 32); err == nil && v > 0 && v>>24 == 20 && i < 4 {
				for len(d.Parts) <= i {
					d.Parts = append(d.Parts, 0)
				}
				d.Parts[i] = int32(v)
			}
		}
	}
	for _, f := range groups {
		v, err := strconv.ParseInt(strings.TrimSpace(f), 10, 32)
		if err != nil || v <= 0 {
			continue
		}
		switch v >> 24 {
		case 21:
			d.Emblem = int32(v)
		case 22:
			d.Paint = int32(v)
		case 23:
			d.Pattern = int32(v)
		case 24:
			d.Decal = int32(v)
		}
	}
	d.ID = 1 // validate checks the saved-boss range; the clone's id comes later
	err := d.validate()
	d.ID = 0
	return d, err
}

func abs(v int) int {
	if v < 0 {
		return -v
	}
	return v
}

// cmdClone: /clone <player> [ship] [1-10] -- the player's flagship (or the
// named ship of theirs) as an enemy boss, "Evil <player>".
func cmdClone(c *chatCommandCtx) string {
	const usage = "Usage: /clone <player|me> [ship] [1-10]"
	if len(c.args) == 0 {
		return usage
	}
	args := append([]string(nil), c.args...)
	mult := 2 // a stock clone loses to the player it copies
	if v, err := strconv.Atoi(strings.TrimPrefix(strings.ToLower(args[len(args)-1]), "x")); err == nil && len(args) > 1 {
		if v < 1 || v > 10 {
			return usage
		}
		mult, args = v, args[:len(args)-1]
	}
	var pid, name string
	if strings.EqualFold(args[0], "me") {
		pid = c.pid
		for _, p := range socialHubInstance.onlinePlayers() {
			if normalizedPlayerStatePID(p.PID) == normalizedPlayerStatePID(c.pid) {
				name = p.Name
			}
		}
	} else {
		p, refuse := onlinePlayerByName(args[0])
		if refuse != "" {
			return refuse
		}
		pid, name = p.PID, p.Name
	}
	if name == "" {
		if name = adminPlayerName(pid); name == "?" {
			name = "Twin"
		}
	}
	var l mmogShipLoadoutSeed
	ok := false
	if len(args) > 1 {
		want := strings.ToLower(strings.Join(args[1:], " "))
		for _, x := range withLoadoutVariants(pid, ownedShipLoadoutsForPlayerData(mmogPlayerStateForPID(pid), pid)) {
			if x.variant == 0 && strings.EqualFold(x.loadoutName, want) {
				l, ok = x, true
			}
		}
		if !ok {
			return fmt.Sprintf("%s has no ship named %s.", name, strings.Join(args[1:], " "))
		}
	} else {
		l, ok = battlePickedLoadoutFor(pid)
		if !ok {
			return name + " has no ships."
		}
	}
	d, err := cloneBossDef(name, l)
	if err != nil {
		return "Cannot clone " + l.loadoutName + ": " + err.Error() + "."
	}
	target, where, ok := matchCommandTarget(c)
	if !ok {
		return noLiveBattleReply("/clone")
	}
	cloneStore.Lock()
	d.ID = cloneStore.next
	cloneStore.next++
	cloneStore.defs[d.ID] = d
	for id := range cloneStore.defs { // keep the last 50; a host fetches within seconds
		if id < d.ID-50 {
			delete(cloneStore.defs, id)
		}
	}
	cloneStore.Unlock()
	queueHostCommand(target, fmt.Sprintf("bossdef %d %d", d.ID, mult))
	return fmt.Sprintf("%s (%s, x%d) sent to %s.", d.Name, l.loadoutName, mult, where)
}
