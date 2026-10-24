package main

import (
	"bytes"
	"encoding/json"
	"path/filepath"
	"testing"
	"time"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/matchmaker"
	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/protocol"
)

// Season 1's four events take turns, one per period, in table order; the
// running one is what the matchmaker launches.
func TestPvESeasonSchedule(t *testing.T) {
	noPvESeasonPin(t)
	t.Setenv("DN_PVE_SEASON", "")
	t.Setenv("DN_PVE_EVENT_DAYS", "7")
	week := 7 * 24 * time.Hour
	want := []string{"PVE_S1E1", "PVE_S1E2", "PVE_S1E3", "PVE_S1E4", "PVE_S1E1"}
	for i, id := range want {
		_, events, active := pveSchedule(pveScheduleAnchor.Add(time.Duration(i)*week + time.Hour))
		if active < 0 || events[active].id != id {
			t.Fatalf("week %d: running %v, want %s", i, events, id)
		}
	}
	// S1E4 is the Escort episode.
	_, events, active := pveSchedule(pveScheduleAnchor.Add(3*week + time.Hour))
	if row := loadPVETables().events[events[active].id]; row.GameMode != "YGMT_ESCORT" {
		t.Errorf("S1E4 mode %s", row.GameMode)
	}
	t.Setenv("DN_PVE_SEASON", "off")
	if _, ok := activePvEEvent(); ok {
		t.Error("an event runs with the season switched off")
	}
}

// YA_GetSeasonData names the season and event, with the client's own texts;
// the mode list carries PVE; YA_Connect names the event of a PvE match.
func TestPvESeasonReachesTheClient(t *testing.T) {
	noPvESeasonPin(t)
	t.Setenv("DN_PVE_SEASON", "")
	ev, ok := activePvEEvent()
	if !ok || ev.HostMode == "" || ev.MapPath == "" {
		t.Fatalf("no running event: %+v", ev)
	}
	payload := buildMmogSeasonDataPayload()
	result := extractNamedMmogObject(t, payload, "result")
	if !bytes.Contains(result, protocol.AppendStringField(nil, "CurrentSeason", "PVE_Season1")) ||
		!bytes.Contains(result, protocol.AppendStringField(nil, "ActiveEvent", ev.ID)) {
		t.Fatal("YA_GetSeasonData does not declare the running season and event")
	}
	events, _, _, _, _ := pveSeasonDataJSON(time.Now().UTC())
	var rows []map[string]any
	if err := json.Unmarshal([]byte(events), &rows); err != nil || len(rows) != 4 {
		t.Fatalf("events blob: %v (%d rows)", err, len(rows))
	}
	if rows[0]["m_name"] != "Incident Management" || len(rows[0]["m_rewardLevels"].([]any)) != 9 {
		t.Errorf("first event: %v", rows[0]["m_name"])
	}

	found := false
	for _, m := range matchmaker.GameModeConfigs() {
		found = found || m.Name == matchmaker.PvEGameMode
	}
	if !found {
		t.Error("the mode list lacks PVE while an event runs")
	}

	connect := appendMmogConnectFields(nil, mmogMatchmakingStatus{gameMode: matchmaker.PvEGameMode, mapName: ev.ID,
		serverIP: "127.0.0.1", serverPort: 7900, matchID: "m"})
	if !bytes.Contains(connect, protocol.AppendStringField(nil, "PVEEvent", ev.ID)) {
		t.Error("YA_Connect lacks the PvE event")
	}
}

func TestPvESeasonRotation(t *testing.T) {
	noPvESeasonPin(t)
	t.Setenv("DN_PVE_SEASON", "rotate")
	t.Setenv("DN_PVE_EVENT_DAYS", "7")
	week := 7 * 24 * time.Hour
	n1, n3 := len(pveSeasonEvents("PVE_Season1")), len(pveSeasonEvents("PVE_Season3"))
	if n1 == 0 || n3 == 0 {
		t.Skip("season tables not loaded")
	}
	for i := 0; i < 2*(n1+n3); i++ {
		season, events, active := pveSchedule(pveScheduleAnchor.Add(time.Duration(i)*week + time.Hour))
		k := i % (n1 + n3)
		want, slot := "PVE_Season1", k
		if k >= n1 {
			want, slot = "PVE_Season3", k-n1
		}
		if season != want || active != slot || len(events) != len(pveSeasonEvents(want)) {
			t.Fatalf("week %d: got %s slot %d (%d events), want %s slot %d", i, season, active, len(events), want, slot)
		}
		if ev := events[active]; ev.id != pveSeasonEvents(want)[slot] || !ev.start.Equal(pveScheduleAnchor.Add(time.Duration(i)*week)) {
			t.Fatalf("week %d: active event %s starts %v", i, ev.id, ev.start)
		}
	}
}

// noPvESeasonPin hides a /season pin (data/pve_season.json) from a test,
// and keeps the test's own pins out of data/.
func noPvESeasonPin(t *testing.T) {
	pveSeasonStore.Lock()
	saved, savedLoaded, savedFile := pveSeasonStore.st, pveSeasonStore.loaded, pveSeasonStateFile
	pveSeasonStore.st, pveSeasonStore.loaded = nil, true
	dir := t.TempDir()
	pveSeasonStateFile = func() string { return filepath.Join(dir, "pve_season.json") }
	pveSeasonStore.Unlock()
	t.Cleanup(func() {
		pveSeasonStore.Lock()
		pveSeasonStore.st, pveSeasonStore.loaded, pveSeasonStateFile = saved, savedLoaded, savedFile
		pveSeasonStore.Unlock()
	})
}

// A /season pin holds its episode with no timer; next walks the season, then
// on to the next season.
func TestPvESeasonPin(t *testing.T) {
	noPvESeasonPin(t)
	now := time.Date(2026, 11, 1, 12, 0, 0, 0, time.UTC)
	st := pveSeasonState{Season: "PVE_Season1", Episode: 2, Since: now.Add(-30 * 24 * time.Hour)}
	pveSeasonStore.st = &st
	season, events, active := pveSchedule(now)
	if season != "PVE_Season1" || active != 2 || events[2].id != "PVE_S1E3" {
		t.Fatalf("pinned %s %d %v", season, active, events)
	}
	if !events[2].end.After(now) || !events[2].start.Before(now) {
		t.Errorf("pinned episode window %v-%v does not hold %v", events[2].start, events[2].end, now)
	}
	if s, e := pveNextEpisode("PVE_Season1", 2); s != "PVE_Season1" || e != 3 {
		t.Errorf("next after S1E3: %s %d", s, e)
	}
	if s, e := pveNextEpisode("PVE_Season1", 3); s != "PVE_Season3" || e != 0 {
		t.Errorf("next after S1E4: %s %d", s, e)
	}
	pveSeasonStore.st = &pveSeasonState{}
	if _, ok := activePvEEvent(); ok {
		t.Error("an event runs with the season pinned off")
	}
}
