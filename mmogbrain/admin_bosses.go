package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"

	"github.com/gorilla/mux"

	dreadconfig "github.com/darkace1998/Dreadnought-Revival-project/shared/dreadgameconfig"
)

// Custom bosses (operator 2026-10-19): the admin dashboard's Bosses tab builds
// numbered boss definitions, and "/boss <id> [mult]" spawns one. A definition
// is picked tier -> class -> manufacturer, which fixes the hull line (each
// maker builds exactly one size per class, baseShipManufacturerByClassSize),
// then a base: one of the Havoc dataset ShipIds of that hull line. The base is
// what the AI really is -- its AI pawn (outside Havoc the class's
// VH_<Cls><W>_AI_Pawn_T<n>, CommandSpawn), its controller behaviour and its
// stock modules. On top of it the definition sets the name tag, the modules
// (ability classes, SetAbilities on the pawn), the coatings (the pawn's
// display info "parts;emblem;paint;pattern;decal") and Havoc boost feats with
// stack counts, applied after the multiplier's own HP/damage stacks.
//
// The battle host cannot take a definition inline (its command line is
// sscanf'd into 63-byte fields), so the chat command queues "bossdef <id>
// <mult>" and the host fetches GET /battle/boss?id=<id> over loopback.
//
// Definitions live in <data>/bosses/bosses.json, written whole on each change.

type bossFeat struct {
	Name   string `json:"name"`
	Stacks int    `json:"stacks"`
}

type bossDef struct {
	ID           int        `json:"id"`
	Name         string     `json:"name"`
	Tier         string     `json:"tier"`              // recruit | veteran | legendary
	Class        string     `json:"class"`             // Assault, Dreadnought, Scout, Sniper, Support
	Manufacturer string     `json:"manufacturer"`      // JupiterArms, AkulaVektor, Oberon
	Base         string     `json:"base"`              // Havoc ShipId (havocEnemyIDs)
	Mult         int        `json:"mult"`              // default multiplier, "/boss <id> <n>" overrides
	Modules      []string   `json:"modules"`           // ability class paths by slot: Primary, Secondary, Perimeter, Internal, Extra 1-4 ("" = none); empty = the base's own
	Weapons      []string   `json:"weapons,omitempty"` // primary, secondary weapon blueprints (WP_*_BP); empty = the base's own
	Paint        int32      `json:"paint"`             // 0 = keep (elites without one are painted gold)
	Pattern      int32      `json:"pattern"`
	Decal        int32      `json:"decal"`
	Emblem       int32      `json:"emblem"`
	Feats        []bossFeat `json:"feats"`
	Parts        []int32    `json:"parts,omitempty"`    // hero ship parts Forecastle, Hull, Bridge, Stern; 0 = keep
	Officers     []string   `json:"officers,omitempty"` // briefing feats (DN_Officers_OTS_DT rows), one per officer
	Notes        string     `json:"notes,omitempty"`
	// Marker: HUD marker over a boss. "" = HostileTarget (the old fixed
	// marker), "None", "Spotted" (the pawn's replicated m_isSpotted, what a
	// player's mark sets), or a DN_QuestMarker_DT row (EYUIQuestMarker name).
	// Hunters carry none.
	Marker string `json:"marker,omitempty"`
}

// bossMarkers: EYUIQuestMarker values by DN_QuestMarker_DT row name (enum
// order in the client's reflection data; 0 = Remove). Host specials:
// -2 none, 100 spotted (dn_host_loadout.cpp MARKER_*).
var bossMarkers = map[string]int{"HostileTarget": 1, "PrimaryTarget": 2, "ProtectionTarget": 3, "FollowTarget": 4,
	"TargetDestination": 5, "Regroup": 6, "DestroyTarget": 7, "OptionalTarget": 8, "LastTargetAlive": 9, "Incoming": 10,
	"None": -2, "Spotted": 100}

var bossTierGroups = []struct {
	ID, Label string
	Tiers     []int
}{
	{"recruit", "Recruit", []int{1, 2}},
	{"veteran", "Veteran", []int{2, 3}},
	{"legendary", "Legendary", []int{4, 5}},
}

var bossClasses = []struct{ ID, Label string }{
	{"Dreadnought", "Dreadnought"}, {"Assault", "Destroyer"}, {"Sniper", "Artillery Cruiser"},
	{"Support", "Tactical Cruiser"}, {"Scout", "Corvette"},
}

var efsCall = regexp.MustCompile(`EFS\((\w+)\)`)
var perkTypePattern = regexp.MustCompile(`^PRK_(COM|ENG|NAV|WPN)_`)
var bossOfficerPattern = regexp.MustCompile(`^[A-Z0-9]{2,30}$`)

var heroPartName = regexp.MustCompile(`(?i)^V(?:AN|H)_H_(Assault|Dread|Scout|Sniper|Support)([HML])_`)
var exportPrefix = regexp.MustCompile(`^VAN_(?:PN|PTN|DCL|H?EMB|H|FH)_(?:(?:Assault|Dread|Scout|Sniper|Support)[HML]_)?|_DA$`)
var camelBreak = regexp.MustCompile(`([a-z])([A-Z0-9])`)

// prettyExport: "VAN_PN_Red_Gold_DA" -> "Red Gold", "VAN_H_DreadH_Aegis_Bridge_DA" -> "Aegis Bridge".
func prettyExport(s string) string {
	t := exportPrefix.ReplaceAllString(s, "")
	t = camelBreak.ReplaceAllString(strings.ReplaceAll(t, "_", " "), "$1 $2")
	if t = strings.Join(strings.Fields(t), " "); t == "" {
		return s
	}
	return t
}

var heroPartLine = map[string]string{"assault": "Assault", "dread": "Dreadnought", "scout": "Scout", "sniper": "Sniper", "support": "Support"}

var bossManufacturers = []string{"JupiterArms", "AkulaVektor", "Oberon"}

const bossMaxModules = 8 // NpcAbilities rows hold 8 slots (npc_abilities.h)
const bossMaxFeats = 12

var bossStore = struct {
	sync.Mutex
	loaded bool
	defs   map[int]bossDef
}{}

func bossFile() string { return filepath.Join(dreadconfig.DataDir(), "bosses", "bosses.json") }

func bossLoadLocked() {
	if bossStore.loaded {
		return
	}
	bossStore.loaded = true
	bossStore.defs = map[int]bossDef{}
	raw, err := os.ReadFile(bossFile())
	if err != nil {
		return
	}
	var list []bossDef
	if json.Unmarshal(raw, &list) == nil {
		for _, d := range list {
			bossStore.defs[d.ID] = d
		}
	}
}

func bossSaveLocked() error {
	list := bossListLocked()
	raw, err := json.MarshalIndent(list, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(bossFile()), 0o755); err != nil {
		return err
	}
	tmp := bossFile() + ".tmp"
	if err := os.WriteFile(tmp, append(raw, '\n'), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, bossFile())
}

func bossListLocked() []bossDef {
	list := make([]bossDef, 0, len(bossStore.defs))
	for _, d := range bossStore.defs {
		list = append(list, d)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].ID < list[j].ID })
	return list
}

func bossByID(id int) (bossDef, bool) {
	if id >= cloneIDBase {
		return cloneByID(id)
	}
	bossStore.Lock()
	defer bossStore.Unlock()
	bossLoadLocked()
	d, ok := bossStore.defs[id]
	return d, ok
}

func bossList() []bossDef {
	bossStore.Lock()
	defer bossStore.Unlock()
	bossLoadLocked()
	return bossListLocked()
}

var shipIDTierPattern = regexp.MustCompile(`T(\d)$`)

// bossBaseTier is a ShipId's tier; the *Boss01 rows count as 5.
func bossBaseTier(ship string) int {
	if strings.HasSuffix(ship, "Boss01") {
		return 5
	}
	if m := shipIDTierPattern.FindStringSubmatch(ship); m != nil {
		n, _ := strconv.Atoi(m[1])
		return n
	}
	return 0
}

func bossHullLine(class, manufacturer string) string {
	for _, size := range []string{"Light", "Medium", "Heavy"} {
		if baseShipManufacturerByClassSize[class+size] == manufacturer {
			return class + size
		}
	}
	return ""
}

func bossTierAllows(tier string, n int) bool {
	for _, g := range bossTierGroups {
		if g.ID == tier {
			for _, t := range g.Tiers {
				if t == n {
					return true
				}
			}
		}
	}
	return false
}

var bossAbilityPathPattern = regexp.MustCompile(`^/Game/[A-Za-z0-9_/]+\.[A-Za-z0-9_]+_C$`)
var bossFeatPattern = regexp.MustCompile(`^HVC_Boost_[A-Za-z0-9_]+$`)
var bossWeaponFile = regexp.MustCompile(`Generic/Weapons/(Assault|Dreadnought|Scout|Sniper|Support)/(Heavy|Medium|Light|SecShort|SecMid|SecLong)/BP/T\d/(WP_\w+_T(\d)_BP)\.uasset$`)
var bossWeaponPattern = regexp.MustCompile(`^WP_[A-Za-z0-9_]+_BP$`)

// validate normalises d and reports the first problem.
func (d *bossDef) validate() error {
	d.Name = strings.TrimSpace(d.Name)
	if d.ID < 1 || d.ID > 999 {
		return fmt.Errorf("boss id must be 1-999")
	}
	if d.Name == "" || len([]rune(d.Name)) > 30 {
		return fmt.Errorf("name must be 1-30 characters")
	}
	if strings.ContainsAny(d.Name, "\r\n=") {
		return fmt.Errorf("name may not contain line breaks or '='")
	}
	if _, ok := bossMarkers[d.Marker]; d.Marker != "" && !ok {
		return fmt.Errorf("unknown marker %q", d.Marker)
	}
	hull := bossHullLine(d.Class, d.Manufacturer)
	if hull == "" {
		return fmt.Errorf("unknown class/manufacturer %q/%q", d.Class, d.Manufacturer)
	}
	known := false
	for _, id := range havocEnemyIDs {
		known = known || id == d.Base
	}
	if !known || !strings.HasPrefix(d.Base, hull) {
		return fmt.Errorf("base %q is not a %s ship", d.Base, hull)
	}
	if !bossTierAllows(d.Tier, bossBaseTier(d.Base)) {
		return fmt.Errorf("base %q (T%d) is outside the %s tiers", d.Base, bossBaseTier(d.Base), d.Tier)
	}
	if d.Mult < 1 || d.Mult > 10 {
		return fmt.Errorf("multiplier must be 1-10")
	}
	if len(d.Modules) > bossMaxModules {
		return fmt.Errorf("at most %d modules", bossMaxModules)
	}
	// Positions are slots (the editor's columns), so empties inside stay; trailing ones go.
	for i, m := range d.Modules {
		if m = strings.TrimSpace(m); m != "" && (!bossAbilityPathPattern.MatchString(m) || len(m) > 190) {
			return fmt.Errorf("module %q is not an ability class path", m)
		}
		d.Modules[i] = strings.TrimSpace(d.Modules[i])
	}
	for len(d.Modules) > 0 && d.Modules[len(d.Modules)-1] == "" {
		d.Modules = d.Modules[:len(d.Modules)-1]
	}
	if len(d.Weapons) > 2 {
		return fmt.Errorf("at most 2 weapons")
	}
	for _, w := range d.Weapons {
		if w != "" && !bossWeaponPattern.MatchString(w) {
			return fmt.Errorf("weapon %q is not a weapon blueprint", w)
		}
	}
	for len(d.Weapons) > 0 && d.Weapons[len(d.Weapons)-1] == "" {
		d.Weapons = d.Weapons[:len(d.Weapons)-1]
	}
	cats := []struct {
		v   int32
		cat int32
		nm  string
	}{{d.Paint, 22, "paint"}, {d.Pattern, 23, "pattern"}, {d.Decal, 24, "decal"}, {d.Emblem, 21, "emblem"}}
	for _, c := range cats {
		if c.v != 0 && (c.v>>24)&0xff != c.cat {
			return fmt.Errorf("%s %d is not a %s item (category %d)", c.nm, c.v, c.nm, c.cat)
		}
	}
	if len(d.Feats) > bossMaxFeats {
		return fmt.Errorf("at most %d feats", bossMaxFeats)
	}
	feats := d.Feats[:0]
	for _, f := range d.Feats {
		if f.Stacks == 0 {
			continue
		}
		if !bossFeatPattern.MatchString(f.Name) || f.Stacks < 1 || f.Stacks > 40 {
			return fmt.Errorf("feat %q x%d: name must be a Havoc boost feat, stacks 1-40", f.Name, f.Stacks)
		}
		feats = append(feats, f)
	}
	d.Feats = feats
	if len(d.Parts) > 4 {
		return fmt.Errorf("at most 4 ship parts")
	}
	for _, p := range d.Parts {
		if p != 0 && (p>>24)&0xff != 20 {
			return fmt.Errorf("part %d is not a ship part (category 20)", p)
		}
	}
	if len(d.Officers) > 4 {
		return fmt.Errorf("at most 4 officer briefings")
	}
	offs := d.Officers[:0]
	for _, o := range d.Officers {
		if o == "" {
			continue
		}
		if !bossOfficerPattern.MatchString(o) {
			return fmt.Errorf("officer briefing %q is not a briefing feat", o)
		}
		offs = append(offs, o)
	}
	d.Officers = offs
	if d.Modules == nil {
		d.Modules = []string{}
	}
	if d.Feats == nil {
		d.Feats = []bossFeat{}
	}
	return nil
}

// ---- catalog ------------------------------------------------------------

type bossCatalogItem struct {
	ID    int32  `json:"id,omitempty"`
	Path  string `json:"path,omitempty"`
	Name  string `json:"name"`
	Slot  string `json:"slot,omitempty"`
	Class string `json:"class,omitempty"`
	Tier  int    `json:"tier,omitempty"`
	Note  string `json:"note,omitempty"`
	// Effects: officers' feat effects (DN_Officers_OTS_DT m_effects).
	Effects string `json:"effects,omitempty"`
	// Triggers: officers' feat triggers (m_triggers), e.g. BeShieldHitPerHitPoint().
	Triggers string `json:"triggers,omitempty"`
}

type bossCatalogBase struct {
	ID       string   `json:"id"`
	HullLine string   `json:"hull_line"`
	Tier     int      `json:"tier"`
	Modules  []string `json:"modules"`   // the dataset row's ability classes
	Weapons  []string `json:"weapons"`   // its primary/secondary weapon blueprints (WP_*_BP)
	Hulls    []string `json:"hulls"`     // player hulls of that line and tier, for flavour
	AIPawnT  bool     `json:"ai_pawn_t"` // a VH_<Cls><W>_AI_Pawn_T<n> exists for its tier
}

type bossCatalog struct {
	Tiers         any                          `json:"tiers"`
	Classes       any                          `json:"classes"`
	Manufacturers []bossCatalogItem            `json:"manufacturers"`
	HullLines     map[string]map[string]string `json:"hull_lines"` // class -> maker -> hull line
	Bases         []bossCatalogBase            `json:"bases"`
	Modules       []bossCatalogItem            `json:"modules"`
	Paints        []bossCatalogItem            `json:"paints"`
	Patterns      []bossCatalogItem            `json:"patterns"`
	Decals        []bossCatalogItem            `json:"decals"`
	Emblems       []bossCatalogItem            `json:"emblems"`
	Feats         []bossCatalogItem            `json:"feats"`
	Parts         []bossCatalogItem            `json:"parts"`    // Slot forecastle|hull|bridge|stern, Class = hull line
	Officers      []bossCatalogItem            `json:"officers"` // Path = feat row, Slot = com|eng|nav|wpn
	Weapons       []bossCatalogItem            `json:"weapons"`  // Path = WP_*_BP, Slot primary|secondary, Note = primary hull weight H|M|L
	// SharedFeats: the rows officer effects call through EFS(<row>), per class
	// ("" = DN_Feats_Shared_OTS_DT, else DN_Feats_<Class>_Shared_OTS_DT; the
	// class table wins). Values differ by class: RetaliatorFeat's cooldown cut
	// per shield HP is 0.00065 s Assault, 0.0009 Sniper, 0.001 Support.
	SharedFeats map[string]map[string]bossFeatRow `json:"shared_feats"`
}

type bossFeatRow struct {
	Triggers string `json:"triggers"`
	Effects  string `json:"effects"`
}

var bossCatalogOnce struct {
	sync.Once
	c bossCatalog
}

// AI pawn tiers the client ships (VH_<Cls><W>_AI_Pawn_T<n>_BP, from the
// cooked asset list); the host falls back to the tierless pawn otherwise.
var bossAIPawnTiers = map[string][]int{
	"AssaultHeavy": {3, 5}, "AssaultLight": {5}, "AssaultMedium": {1, 2, 3, 5},
	"DreadnoughtHeavy": {5}, "DreadnoughtLight": {3, 5}, "DreadnoughtMedium": {1, 2, 3, 5},
	"ScoutHeavy": {3, 5}, "ScoutLight": {2, 3, 5}, "ScoutMedium": {3, 5},
	"SniperHeavy": {3, 5}, "SniperLight": {2, 3, 5}, "SniperMedium": {1, 2, 3, 5},
	"SupportHeavy": {5}, "SupportLight": {3, 5}, "SupportMedium": {1, 2, 3, 5},
}

var abilityTierPattern = regexp.MustCompile(`_T(\d)_`)
var abilitySlotPattern = regexp.MustCompile(`^AB_[A-Z]+_(Pri|Sec|Per|Int)_`)

func bossClassPath(assetPath string) string {
	return assetPath + "." + path.Base(assetPath) + "_C"
}

func bossAbilityItem(classPath string, id int32) bossCatalogItem {
	asset := strings.SplitN(classPath, ".", 2)[0]
	base := path.Base(asset)
	it := bossCatalogItem{ID: id, Path: classPath}
	parts := strings.Split(asset, "/")
	if len(parts) > 4 {
		it.Class = parts[4]
	}
	if m := abilitySlotPattern.FindStringSubmatch(base); m != nil {
		it.Slot = map[string]string{"Pri": "primary", "Sec": "secondary", "Per": "perimeter", "Int": "internal"}[m[1]]
	}
	if m := abilityTierPattern.FindStringSubmatch(base); m != nil {
		it.Tier, _ = strconv.Atoi(m[1])
	}
	if name, ok := authoritativeItemName(id); ok && id != 0 {
		it.Name = name
	} else {
		it.Name = base
	}
	return it
}

func readJSONLines(file string, each func([]byte)) {
	f, err := os.Open(file)
	if err != nil {
		return
	}
	defer func() { _ = f.Close() }()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 16<<20)
	for sc.Scan() {
		each(sc.Bytes())
	}
}

func buildBossCatalog() bossCatalog {
	dir := dreadconfig.DataDir()
	c := bossCatalog{HullLines: map[string]map[string]string{}}
	c.Tiers = bossTierGroups
	c.Classes = bossClasses
	for _, m := range bossManufacturers {
		c.Manufacturers = append(c.Manufacturers, bossCatalogItem{Path: m, Name: gatewayManufacturerDisplayName(m)})
	}
	for _, cl := range bossClasses {
		c.HullLines[cl.ID] = map[string]string{}
		for _, m := range bossManufacturers {
			c.HullLines[cl.ID][m] = bossHullLine(cl.ID, m)
		}
	}

	// Item register: ability classes (category 4) and the coatings.
	var reg struct {
		ItemIDRegister []struct {
			ItemID int32
			Path   string
		}
	}
	pathID := map[string]int32{}
	if raw, err := os.ReadFile(filepath.Join(dir, "assets", "ItemIDRegister.json")); err == nil && json.Unmarshal(raw, &reg) == nil {
		for _, it := range reg.ItemIDRegister {
			pathID[it.Path] = it.ItemID
			if (it.ItemID>>24)&0xff == 4 && strings.HasPrefix(it.Path, "/Game/Generic/Abilities/") {
				ab := bossAbilityItem(bossClassPath(it.Path), it.ItemID)
				if ab.Slot != "" {
					c.Modules = append(c.Modules, ab)
				}
			}
		}
	}

	// Player weapons: Generic/Weapons/<Class>/<Heavy|Medium|Light|SecShort|SecMid|SecLong>/BP/T<n>/WP_*_T<n>_BP.
	// The untiered WP_*_BP twins are skipped: no tier to file them under.
	readJSONLines(filepath.Join(dir, "loadouts", "ItemHeadlines_cooked.jsonl"), func(line []byte) {
		var it struct {
			File string `json:"file"`
			ID   int32  `json:"m_itemID"`
		}
		if json.Unmarshal(line, &it) != nil {
			return
		}
		m := bossWeaponFile.FindStringSubmatch(it.File)
		if m == nil {
			return
		}
		tier, _ := strconv.Atoi(m[4])
		w := bossCatalogItem{ID: it.ID, Path: m[3], Name: m[3], Class: m[1], Tier: tier, Slot: "secondary"}
		if !strings.HasPrefix(m[2], "Sec") {
			w.Slot, w.Note = "primary", m[2][:1]
		}
		c.Weapons = append(c.Weapons, w)
	})
	sort.Slice(c.Weapons, func(i, j int) bool { return c.Weapons[i].Path < c.Weapons[j].Path })

	// Havoc dataset rows: each base's stock modules.
	rowModules, rowWeapons := map[string][]string{}, map[string][]string{}
	readJSONLines(filepath.Join(dir, "pve", "HVC_DeadHorde_EnemyShips_rows.jsonl"), func(line []byte) {
		var doc struct {
			Pawns []map[string]any `json:"m_PawnsData"`
		}
		if json.Unmarshal(line, &doc) != nil {
			return
		}
		for _, p := range doc.Pawns {
			id, _ := p["m_shipID"].(string)
			var mods []string
			for _, k := range []string{"m_primaryAbility", "m_secondaryAbility", "m_perimeterAbility", "m_internalAbility",
				"m_extraNPCAbility1", "m_extraNPCAbility2", "m_extraNPCAbility3", "m_extraNPCAbility4"} {
				if s, _ := p[k].(string); s != "" {
					mods = append(mods, s)
				}
			}
			var wps []string
			for _, k := range []string{"m_primaryWeapon", "m_secondaryWeapon"} {
				if s, _ := p[k].(string); s != "" {
					wps = append(wps, path.Base(strings.SplitN(s, ".", 2)[0]))
				}
			}
			if id != "" {
				rowModules[id], rowWeapons[id] = mods, wps
			}
		}
	})
	seen := map[string]bool{}
	for _, m := range c.Modules {
		seen[m.Path] = true
	}
	for _, id := range havocEnemyIDs {
		hull := ""
		for line := range baseShipManufacturerByClassSize {
			if strings.HasPrefix(id, line) {
				hull = line
			}
		}
		if hull == "" {
			continue // TitanCarrier: no class/size, no AI pawn outside Havoc
		}
		b := bossCatalogBase{ID: id, HullLine: hull, Tier: bossBaseTier(id), Modules: rowModules[id], Weapons: rowWeapons[id], Hulls: []string{}}
		if b.Weapons == nil {
			b.Weapons = []string{}
		}
		if b.Modules == nil {
			b.Modules = []string{}
		}
		for _, t := range bossAIPawnTiers[hull] {
			b.AIPawnT = b.AIPawnT || t == b.Tier
		}
		for _, h := range baseShipLoadouts {
			if h.hullLine == hull && int(h.tier) == b.Tier {
				b.Hulls = append(b.Hulls, h.name)
			}
		}
		c.Bases = append(c.Bases, b)
		// NPC-only ability classes (often T5 variants the register lacks).
		for _, m := range b.Modules {
			if !seen[m] {
				seen[m] = true
				it := bossAbilityItem(m, pathID[strings.SplitN(m, ".", 2)[0]])
				it.Note = "Havoc NPC module"
				c.Modules = append(c.Modules, it)
			}
		}
	}
	sort.Slice(c.Modules, func(i, j int) bool {
		a, b := c.Modules[i], c.Modules[j]
		if a.Class != b.Class {
			return a.Class < b.Class
		}
		if a.Slot != b.Slot {
			return a.Slot < b.Slot
		}
		if a.Name != b.Name {
			return a.Name < b.Name
		}
		return a.Tier < b.Tier
	})

	// Coatings: the cooked vanity items, names from VanityTexts (English).
	var texts map[string]struct {
		Name map[string]string `json:"name"`
	}
	if raw, err := os.ReadFile(filepath.Join(dir, "vanity", "VanityTexts.json")); err == nil {
		_ = json.Unmarshal(raw, &texts)
	}
	readJSONLines(filepath.Join(dir, "vanity", "VanityItems_cooked.jsonl"), func(line []byte) {
		var v struct {
			Export string `json:"export"`
			Meta   struct {
				Internal bool `json:"m_internalOnly"`
			} `json:"m_itemMetaData"`
			Sys struct {
				ID int32 `json:"m_itemID"`
			} `json:"m_itemSystemData"`
		}
		if json.Unmarshal(line, &v) != nil || v.Sys.ID == 0 {
			return
		}
		it := bossCatalogItem{ID: v.Sys.ID, Name: v.Export}
		if n, ok := authoritativeItemName(v.Sys.ID); ok && n != "" {
			it.Name = n
		} else if t, ok := texts[strconv.Itoa(int(v.Sys.ID))]; ok && t.Name["en"] != "" {
			it.Name = t.Name["en"]
		}
		if it.Name == v.Export {
			// No text in the item tables or the locres (the asset's m_headline
			// key has no en entry): the export name made readable.
			it.Name = prettyExport(v.Export)
		}
		if it.Name != v.Export {
			it.Note = v.Export
		}
		if v.Meta.Internal {
			it.Note = strings.TrimSpace(it.Note + " (internal)")
		}
		switch (v.Sys.ID >> 24) & 0xff {
		case 20:
			// Hero ship parts: VAN_H_<Cls><W>_..._<Forecastle|Hull|Bridge|Quarterdeck|Stern>_..._DA
			// (figurehead variants are forecastles, VAN_FH_ ones are separate
			// figurehead meshes the loadout parts do not name).
			m := heroPartName.FindStringSubmatch(v.Export)
			if m == nil || strings.HasPrefix(v.Export, "VAN_FH_") {
				return
			}
			it.Class = heroPartLine[strings.ToLower(m[1])] + map[string]string{"H": "Heavy", "M": "Medium", "L": "Light"}[strings.ToUpper(m[2])]
			lo := strings.ToLower(v.Export)
			switch {
			case strings.Contains(lo, "forecastle"):
				it.Slot = "forecastle"
			case strings.Contains(lo, "hull"):
				it.Slot = "hull"
			case strings.Contains(lo, "bridge"), strings.Contains(lo, "deck"):
				it.Slot = "bridge"
			case strings.Contains(lo, "stern"):
				it.Slot = "stern"
			default:
				return
			}
			c.Parts = append(c.Parts, it)
		case 21:
			c.Emblems = append(c.Emblems, it)
		case 22:
			c.Paints = append(c.Paints, it)
		case 23:
			c.Patterns = append(c.Patterns, it)
		case 24:
			c.Decals = append(c.Decals, it)
		}
	})
	for _, l := range [][]bossCatalogItem{c.Emblems, c.Paints, c.Patterns, c.Decals, c.Parts} {
		sort.Slice(l, func(i, j int) bool { return strings.ToLower(l[i].Name) < strings.ToLower(l[j].Name) })
	}

	// Feats: the Havoc boosts the battle host can add (shared OTS table,
	// what FeatOp finds), effects text as the description.
	var feats struct {
		Rows map[string]struct {
			Effects string `json:"m_effects"`
		} `json:"rows"`
	}
	var boosts struct {
		Rows map[string]struct {
			Feats    string `json:"m_feats"`
			Category string `json:"m_category"`
		} `json:"rows"`
	}
	if raw, err := os.ReadFile(filepath.Join(dir, "datatables", "ShipFeats", "DN_Feats_Havoc_Boosts_Shared_OTS_DT.json")); err == nil {
		_ = json.Unmarshal(raw, &feats)
	}
	if raw, err := os.ReadFile(filepath.Join(dir, "datatables", "Progression", "Havoc", "DN_HavocBoosts_DT.json")); err == nil {
		_ = json.Unmarshal(raw, &boosts)
	}
	cat := map[string]string{}
	for _, b := range boosts.Rows {
		s := b.Category
		if i := strings.LastIndex(s, "_"); i >= 0 {
			s = strings.ToLower(s[i+1:])
		}
		cat[b.Feats] = s
	}
	for name, f := range feats.Rows {
		c.Feats = append(c.Feats, bossCatalogItem{Path: name, Name: strings.TrimPrefix(name, "HVC_Boost_"), Slot: cat[name],
			Note: strings.TrimSpace(f.Effects)})
	}
	// Officer briefings: the PRK_*_BP defaults (data/loadouts/Perks_cooked.jsonl,
	// bpdump --loadouts Generic/Officer/Perk), the player-facing ones only. The
	// type is the blueprint's PRK_<COM|ENG|NAV|WPN>_ prefix, the officer slot it
	// fits (Havoc rows: m_officerFirstPerk = communications, Engineer, Navigation,
	// Weapon).
	var offFeats struct {
		Rows map[string]struct {
			Triggers string `json:"m_triggers"`
			Effects  string `json:"m_effects"`
		} `json:"rows"`
	}
	if raw, err := os.ReadFile(filepath.Join(dir, "datatables", "DN_Officers_OTS_DT.json")); err == nil {
		_ = json.Unmarshal(raw, &offFeats)
	}
	readJSONLines(filepath.Join(dir, "loadouts", "Perks_cooked.jsonl"), func(line []byte) {
		var p struct {
			File string `json:"file"`
			Name string `json:"m_perkName"`
			Desc string `json:"m_description"`
			Feat string `json:"m_perkFeatName"`
			Sys  struct {
				ID int32 `json:"m_itemID"`
			} `json:"m_itemSystemData"`
		}
		if json.Unmarshal(line, &p) != nil || p.Feat == "" || !isOfficerBriefing(p.Sys.ID) {
			return
		}
		slot := ""
		if m := perkTypePattern.FindStringSubmatch(path.Base(p.File)); m != nil {
			slot = strings.ToLower(m[1])
		}
		name := p.Name
		if n, ok := authoritativeItemName(p.Sys.ID); ok && n != "" {
			name = n
		}
		c.Officers = append(c.Officers, bossCatalogItem{ID: p.Sys.ID, Path: p.Feat, Name: name, Slot: slot, Note: p.Desc,
			Effects: strings.TrimSpace(offFeats.Rows[p.Feat].Effects), Triggers: strings.TrimSpace(offFeats.Rows[p.Feat].Triggers)})
	})
	c.SharedFeats = map[string]map[string]bossFeatRow{}
	want := map[string]bool{}
	for _, row := range offFeats.Rows {
		for _, m := range efsCall.FindAllStringSubmatch(row.Effects, -1) {
			want[m[1]] = true
		}
	}
	for _, cls := range append([]string{""}, "Assault", "Dreadnought", "Scout", "Sniper", "Support") {
		name := "DN_Feats_Shared_OTS_DT.json"
		if cls != "" {
			name = "DN_Feats_" + cls + "_Shared_OTS_DT.json"
		}
		var t struct {
			Rows map[string]struct {
				Triggers string `json:"m_triggers"`
				Effects  string `json:"m_effects"`
			} `json:"rows"`
		}
		raw, err := os.ReadFile(filepath.Join(dir, "datatables", "ShipFeats", name))
		if err != nil || json.Unmarshal(raw, &t) != nil {
			continue
		}
		rows := map[string]bossFeatRow{}
		for k, r := range t.Rows {
			if want[k] {
				rows[k] = bossFeatRow{Triggers: strings.TrimSpace(r.Triggers), Effects: strings.TrimSpace(r.Effects)}
			}
		}
		c.SharedFeats[cls] = rows
	}
	sort.Slice(c.Officers, func(i, j int) bool {
		if c.Officers[i].Slot != c.Officers[j].Slot {
			return c.Officers[i].Slot < c.Officers[j].Slot
		}
		return c.Officers[i].Name < c.Officers[j].Name
	})
	sort.Slice(c.Feats, func(i, j int) bool {
		if c.Feats[i].Slot != c.Feats[j].Slot {
			return c.Feats[i].Slot < c.Feats[j].Slot
		}
		return c.Feats[i].Name < c.Feats[j].Name
	})
	return c
}

func bossCatalogData() bossCatalog {
	bossCatalogOnce.Do(func() { bossCatalogOnce.c = buildBossCatalog() })
	return bossCatalogOnce.c
}

// ---- admin API ----------------------------------------------------------

func registerAdminBosses(api *mux.Router) {
	api.HandleFunc("/bosses", func(w http.ResponseWriter, _ *http.Request) {
		writeAdminJSON(w, map[string]any{"bosses": bossList()})
	}).Methods(http.MethodGet)
	api.HandleFunc("/bosses/catalog", func(w http.ResponseWriter, _ *http.Request) {
		writeAdminJSON(w, bossCatalogData())
	}).Methods(http.MethodGet)
	api.HandleFunc("/bosses/{id:[0-9]+}", adminAPIBossPut).Methods(http.MethodPost, http.MethodPut)
	api.HandleFunc("/bosses/{id:[0-9]+}", adminAPIBossDelete).Methods(http.MethodDelete)
	api.HandleFunc("/bosses/{id:[0-9]+}/spawn", adminAPIBossSpawn).Methods(http.MethodPost)
}

func adminBossError(w http.ResponseWriter, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

// adminAPIBossPut creates or replaces boss {id}; id 0 = a new boss, numbered here. With ?from=<old id> it is a
// renumbering: the old id is removed once the new one is saved.
func adminAPIBossPut(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.Atoi(mux.Vars(r)["id"])
	var d bossDef
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 64<<10)).Decode(&d); err != nil {
		adminBossError(w, http.StatusBadRequest, "bad JSON: "+err.Error())
		return
	}
	bossStore.Lock()
	defer bossStore.Unlock()
	bossLoadLocked()
	if id == 0 { // a new boss: the server numbers it
		for k := range bossStore.defs {
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
	if _, taken := bossStore.defs[id]; taken && from != 0 && from != id {
		adminBossError(w, http.StatusConflict, fmt.Sprintf("boss %d already exists", id))
		return
	}
	prev, had := bossStore.defs[from]
	bossStore.defs[id] = d
	if from != 0 && from != id && had {
		delete(bossStore.defs, from)
	}
	if err := bossSaveLocked(); err != nil {
		delete(bossStore.defs, id)
		if had {
			bossStore.defs[from] = prev
		}
		adminBossError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if db := currentMmogPlayerStateDB(); db != nil {
		adminAudit(db, r, "save boss", strconv.Itoa(id), map[string]any{"name": d.Name, "base": d.Base, "mult": d.Mult})
	}
	writeAdminJSON(w, map[string]any{"bosses": bossListLocked(), "id": id})
}

func adminAPIBossDelete(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.Atoi(mux.Vars(r)["id"])
	bossStore.Lock()
	defer bossStore.Unlock()
	bossLoadLocked()
	d, ok := bossStore.defs[id]
	if !ok {
		adminBossError(w, http.StatusNotFound, "no such boss")
		return
	}
	delete(bossStore.defs, id)
	if err := bossSaveLocked(); err != nil {
		bossStore.defs[id] = d
		adminBossError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if db := currentMmogPlayerStateDB(); db != nil {
		adminAudit(db, r, "delete boss", strconv.Itoa(id), map[string]any{"name": d.Name})
	}
	writeAdminJSON(w, map[string]any{"bosses": bossListLocked()})
}

// adminAPIBossSpawn sends boss {id} to every running match: body {"mult": n}
// (0 = the boss's own).
func adminAPIBossSpawn(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.Atoi(mux.Vars(r)["id"])
	var req struct {
		Mult int `json:"mult"`
	}
	_ = json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&req)
	d, ok := bossByID(id)
	if !ok {
		adminBossError(w, http.StatusNotFound, "no such boss")
		return
	}
	mult := d.Mult
	if req.Mult >= 1 && req.Mult <= 10 {
		mult = req.Mult
	}
	queueHostCommand("", fmt.Sprintf("bossdef %d %d", id, mult))
	writeAdminJSON(w, map[string]any{"sent": fmt.Sprintf("Boss %d %s x%d sent to every running match.", id, d.Name, mult)})
}

// ---- battle host --------------------------------------------------------

// battleBossHandler: GET /battle/boss?id=<n>, loopback only. One key=value
// per line for the host's FieldValue: ship, name, paint, pattern, decal,
// emblem (0 = keep), abilities ("|"-separated class paths, empty = the
// ship's own), feats ("name*stacks|...").
func battleBossHandler(w http.ResponseWriter, r *http.Request) {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		http.Error(w, "loopback only", http.StatusForbidden)
		return
	}
	id, _ := strconv.Atoi(r.URL.Query().Get("id"))
	d, ok := bossByID(id)
	if !ok {
		http.Error(w, "no such boss", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = fmt.Fprint(w, bossHostText(d))
}

func bossHostText(d bossDef) string {
	feats := make([]string, 0, len(d.Feats))
	for _, f := range d.Feats {
		feats = append(feats, fmt.Sprintf("%s*%d", f.Name, f.Stacks))
	}
	mods := make([]string, 0, len(d.Modules))
	for _, m := range d.Modules {
		if m != "" {
			mods = append(mods, m)
		}
	}
	parts := make([]string, 0, 4)
	for _, p := range d.Parts {
		parts = append(parts, strconv.Itoa(int(p)))
	}
	marker := ""
	if v, ok := bossMarkers[d.Marker]; ok {
		marker = strconv.Itoa(v)
	}
	// weapons: not applied by the host yet (it has no NPC weapon swap); carried so it can be.
	return fmt.Sprintf("ok=1\nship=%s\nname=%s\npaint=%d\npattern=%d\ndecal=%d\nemblem=%d\nabilities=%s\nfeats=%s\nparts=%s\nofficers=%s\nweapons=%s\nmarker=%s\n",
		d.Base, d.Name, d.Paint, d.Pattern, d.Decal, d.Emblem, strings.Join(mods, "|"), strings.Join(feats, "|"),
		strings.Join(parts, ","), strings.Join(d.Officers, "|"), strings.Join(d.Weapons, "|"), marker)
}
