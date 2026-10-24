package matchmaker

import (
	"strings"
	"sync"
)

// Rotation: the operator's map and mode switches (mmogbrain's admin Maps tab,
// rotation.json). Keys are map names (GameMap.Name) and mode groups
// (RotationModeKey); a missing key keeps the default: on, except Havoc, which
// follows DN_HAVOC.
var rotation struct {
	sync.RWMutex
	maps, modes map[string]bool
}

// SetRotation replaces the switches.
func SetRotation(maps, modes map[string]bool) {
	rotation.Lock()
	rotation.maps, rotation.modes = maps, modes
	rotation.Unlock()
}

// RotationModeKey folds a client mode name onto its switch: the aliases the
// client lists twice (Territory, Bootcamp) and the queue names share one.
func RotationModeKey(mode string) string {
	switch m := NormalizeGameMode(mode); {
	case m == "Territory":
		return "TER"
	case m == "Bootcamp":
		return "BC"
	case strings.EqualFold(m, havocClientName):
		return HavocGameMode
	default:
		return m
	}
}

// ModeEnabled reports whether a mode is listed and may be queued for.
func ModeEnabled(mode string) bool {
	k := RotationModeKey(mode)
	rotation.RLock()
	on, set := rotation.modes[k]
	rotation.RUnlock()
	if set {
		return on
	}
	return k != HavocGameMode || havocListed()
}

// ModeSwitchedOff reports an explicit off switch: the queue refuses those.
// (Havoc unlisted by default still queues, as before the switches.)
func ModeSwitchedOff(mode string) bool {
	rotation.RLock()
	on, set := rotation.modes[RotationModeKey(mode)]
	rotation.RUnlock()
	return set && !on
}

// MapEnabled reports whether a map is in rotation.
func MapEnabled(name string) bool {
	rotation.RLock()
	on, set := rotation.maps[name]
	rotation.RUnlock()
	return on || !set
}

// rotationMaps drops the switched-off maps from a mode's list; with all of
// them off the list stays whole, so the mode still has somewhere to play.
func rotationMaps(l []GameMap) []GameMap {
	out := make([]GameMap, 0, len(l))
	for _, m := range l {
		if MapEnabled(m.Name) {
			out = append(out, m)
		}
	}
	if len(out) == 0 {
		return l
	}
	return out
}

// RotationMapEntry: a switchable map and the modes that can pick it.
type RotationMapEntry struct {
	Name  string   `json:"name"`
	Path  string   `json:"path"`
	Modes []string `json:"modes"`
}

// RotationMapList: every map a formed match can land on, rotation order.
func RotationMapList() []RotationMapEntry {
	var out []RotationMapEntry
	idx := map[string]int{}
	add := func(m GameMap, mode string) {
		i, ok := idx[m.Name]
		if !ok {
			i, idx[m.Name] = len(out), len(out)
			out = append(out, RotationMapEntry{Name: m.Name, Path: m.Path, Modes: []string{}})
		}
		for _, x := range out[i].Modes {
			if x == mode {
				return
			}
		}
		out[i].Modes = append(out[i].Modes, mode)
	}
	for _, mode := range RotationModeList() {
		for _, m := range modeMaps(mode) {
			add(m, mode)
		}
	}
	return out
}

// RotationModeList: the switchable modes, one per group.
func RotationModeList() []string {
	out := []string{}
	seen := map[string]bool{}
	for _, m := range append(GameModeList(), PvEGameMode, HavocGameMode) {
		if k := RotationModeKey(m); !seen[k] {
			seen[k] = true
			out = append(out, k)
		}
	}
	return out
}

// modeMaps: the maps a mode picks from before the switches.
func modeMaps(mode string) []GameMap {
	if l, ok := mapsByGameMode[mode]; ok {
		return l
	}
	if mode == "BC" {
		return pveMaps
	}
	if mode == PvEGameMode {
		return nil // the running event's map
	}
	return availableMaps
}
