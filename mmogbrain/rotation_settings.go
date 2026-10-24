package main

// Map and mode switches: the admin Maps tab and the launcher's MAPS page
// (DN_ADMINS may switch, everyone else sees them). Stored whole in
// <data>/rotation.json, applied to the matchmaker (matchmaker/rotation.go).
// Turning a mode off unlists it for clients that fetch the game config
// afterwards (sign-in); a queue request for it is refused.

import (
	"bytes"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/matchmaker"
	dreadconfig "github.com/darkace1998/Dreadnought-Revival-project/shared/dreadgameconfig"
	"github.com/gorilla/mux"
)

type rotationSettings struct {
	Maps  map[string]bool `json:"maps"`
	Modes map[string]bool `json:"modes"`
}

var rotationStore struct {
	sync.Mutex
	s rotationSettings
}

func rotationFile() string { return filepath.Join(dreadconfig.DataDir(), "rotation.json") }

// loadRotationSettings: at startup.
func loadRotationSettings() {
	var s rotationSettings
	if raw, err := os.ReadFile(rotationFile()); err == nil {
		_ = json.Unmarshal(raw, &s)
	}
	rotationStore.Lock()
	rotationStore.s = s
	rotationStore.Unlock()
	matchmaker.SetRotation(s.Maps, s.Modes)
}

// saveRotationSettings keeps only known names.
func saveRotationSettings(in rotationSettings) (rotationSettings, error) {
	s := rotationSettings{Maps: map[string]bool{}, Modes: map[string]bool{}}
	for _, m := range matchmaker.RotationMapList() {
		if v, ok := in.Maps[m.Name]; ok {
			s.Maps[m.Name] = v
		}
	}
	for _, m := range matchmaker.RotationModeList() {
		if v, ok := in.Modes[m]; ok {
			s.Modes[m] = v
		}
	}
	raw, _ := json.MarshalIndent(s, "", "  ")
	tmp := rotationFile() + ".tmp"
	if err := os.WriteFile(tmp, append(raw, '\n'), 0o644); err != nil {
		return s, err
	}
	if err := os.Rename(tmp, rotationFile()); err != nil {
		return s, err
	}
	rotationStore.Lock()
	rotationStore.s = s
	rotationStore.Unlock()
	matchmaker.SetRotation(s.Maps, s.Modes)
	return s, nil
}

type rotationMapView struct {
	Name  string   `json:"name"`
	Label string   `json:"label"`
	Art   string   `json:"art,omitempty"` // admin-art path
	Modes []string `json:"modes"`
	On    bool     `json:"on"`
}

type rotationModeView struct {
	ID    string `json:"id"`
	Label string `json:"label"`
	Art   string `json:"art,omitempty"`
	On    bool   `json:"on"`
	// Tutorial: the recruit modes, listed apart from the real ones.
	Tutorial bool `json:"tutorial,omitempty"`
}

func rotationView() map[string]any {
	maps := []rotationMapView{}
	for _, m := range matchmaker.RotationMapList() {
		v := rotationMapView{Name: m.Name, Label: rotationMapLabel(m.Name), Modes: m.Modes, On: matchmaker.MapEnabled(m.Name)}
		if rotationArtExists("maps/" + m.Name + ".jpg") {
			v.Art = "maps/" + m.Name + ".jpg"
		}
		maps = append(maps, v)
	}
	modes := []rotationModeView{}
	for _, m := range matchmaker.RotationModeList() {
		v := rotationModeView{ID: m, Label: rotationModeLabel(m), On: matchmaker.ModeEnabled(m), Tutorial: m == "BC" || m == "TM"}
		if rotationArtExists("modes/" + m + ".png") {
			v.Art = "modes/" + m + ".png"
		}
		modes = append(modes, v)
	}
	sort.SliceStable(modes, func(i, j int) bool { return rotationModeOrder(modes[i].ID) < rotationModeOrder(modes[j].ID) })
	return map[string]any{"maps": maps, "modes": modes}
}

func rotationArtExists(rel string) bool {
	_, err := os.Stat(filepath.Join(dreadconfig.DataDir(), "admin-art", filepath.FromSlash(rel)))
	return err == nil
}

// rotationMapLabel: the rotation name, Space02 as the client calls it (viewerMapName).
func rotationMapLabel(name string) string {
	base, night := strings.CutSuffix(name, "_Night")
	if base == "Space02" {
		base = "Ryugu Haven"
	}
	if night {
		base += " (night)"
	}
	return base
}

// Client names: SID_MATCHMAKINGTYPE_* / GlobalUI (serverChatModeNames), TM the
// Training Match, BC the Proving Grounds button.
func rotationModeLabel(m string) string {
	switch m {
	case "TurboTDM":
		return "Turbo Team Deathmatch"
	case "BC":
		return "Proving Grounds"
	case "TM":
		return "Training Match"
	case "PVE":
		return "PvE"
	}
	return viewerModeName(m)
}

func rotationModeOrder(m string) int {
	for i, x := range []string{"TDM", "TurboTDM", "TE", "TER", "PodTDM", "Onslaught", "Havoc", "PVE", "BC", "TM"} {
		if x == m {
			return i
		}
	}
	return 99
}

func registerAdminRotation(api *mux.Router) {
	api.HandleFunc("/rotation", func(w http.ResponseWriter, _ *http.Request) {
		writeAdminJSON(w, rotationView())
	}).Methods(http.MethodGet)
	api.HandleFunc("/rotation", func(w http.ResponseWriter, r *http.Request) {
		var in rotationSettings
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16384)).Decode(&in); err != nil {
			adminBossError(w, http.StatusBadRequest, "bad JSON: "+err.Error())
			return
		}
		s, err := saveRotationSettings(in)
		if err != nil {
			adminBossError(w, http.StatusInternalServerError, err.Error())
			return
		}
		if db := currentMmogPlayerStateDB(); db != nil {
			adminAudit(db, r, "save map rotation", "", s)
		}
		writeAdminJSON(w, rotationView())
	}).Methods(http.MethodPut, http.MethodPost)
}

// launcherRotation: GET /launcher/rotation (+ "admin": may switch), POST for DN_ADMINS.
func launcherRotation(w http.ResponseWriter, r *http.Request) {
	uid := launcherUID(r)
	if uid == "" {
		http.Error(w, "unavailable", http.StatusNotFound)
		return
	}
	if r.Method == http.MethodPost {
		if !isChatAdmin(uid) {
			launcherFleetError(w, http.StatusForbidden, "admins only")
			return
		}
		var in rotationSettings
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16384)).Decode(&in); err != nil {
			launcherFleetError(w, http.StatusBadRequest, "bad JSON")
			return
		}
		if _, err := saveRotationSettings(in); err != nil {
			launcherFleetError(w, http.StatusInternalServerError, err.Error())
			return
		}
	}
	v := rotationView()
	v["admin"] = isChatAdmin(uid)
	writeAdminJSON(w, v)
}

// launcherRotationPage: GET /launcher/rotation/page -- the dashboard's Maps
// tab alone (DN_ROT), framed by the launcher like the Fleet page.
func launcherRotationPage(w http.ResponseWriter, r *http.Request) {
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
	page = bytes.Replace(page, []byte("<script>"), []byte("<script>window.DN_ROT = {};</script>\n<script>"), 1)
	page = bytes.Replace(page, []byte("<head>"), []byte(`<head><meta name="color-scheme" content="dark"><style>html{background:transparent}</style>`), 1)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write(page)
}
