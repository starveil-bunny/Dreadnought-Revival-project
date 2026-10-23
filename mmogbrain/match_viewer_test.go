package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestViewerSnapshotRoundTrip(t *testing.T) {
	useTempMmogPlayerStateDB(t)
	body := "100 -200 3000 1 87 0 650dd794-76a1-484b-8adc-d01ac2f17354 VH_AssaultM_Pawn_T2_BP_C\n" +
		"5 6 7 2 40 1 - HVC_VH_SniperH_Boss_01_Pawn_BP_C\nbroken line\n"
	req := httptest.NewRequest(http.MethodPost, "/battle/snapshot?match=bm1", strings.NewReader(body))
	req.RemoteAddr = "127.0.0.1:5000"
	rec := httptest.NewRecorder()
	battleSnapshotHandler(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("snapshot: %d", rec.Code)
	}
	req = httptest.NewRequest(http.MethodPost, "/battle/snapshot?match=bm1", strings.NewReader(body))
	req.RemoteAddr = "10.0.0.5:5000"
	rec = httptest.NewRecorder()
	battleSnapshotHandler(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Errorf("non-loopback snapshot: %d", rec.Code)
	}
	r := adminTestRouter()
	if rec := adminGet(r, "/admin/api/battles", ""); rec.Code != http.StatusForbidden {
		t.Errorf("battles without key: %d", rec.Code)
	}
	rec = adminGet(r, "/admin/api/battles/bm1", "test-admin-key")
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"name":"Medium Destroyer T2"`) ||
		!strings.Contains(rec.Body.String(), `"name":"Heavy Artillery Cruiser (boss)"`) || !strings.Contains(rec.Body.String(), `"pid":"650dd79476a1484b8adcd01ac2f17354"`) {
		t.Fatalf("battle: %d %s", rec.Code, rec.Body.String())
	}
	if rec := adminPost(r, "/admin/api/battles/bm1/command", "test-admin-key", `{"line":"/nuke"}`); rec.Code != http.StatusNotFound {
		t.Errorf("command without a match record: %d %s", rec.Code, rec.Body.String())
	}
}

func TestViewerShipKind(t *testing.T) {
	for class, want := range map[string][2]string{
		"VH_SniperL_AI_Pawn_T2_BP_C":       {"ArtilleryCruiser", "Light Artillery Cruiser T2"},
		"VH_AssaultM_Pawn_T1_BP_C":         {"Destroyer", "Medium Destroyer T1"},
		"HVC_VH_SniperH_Boss_01_Pawn_BP_C": {"ArtilleryCruiser", "Heavy Artillery Cruiser (boss)"},
		"VH_ONS_AITargetM_Pawn_BP_C":       {"AttackBoat", "Attack Boat"},
		"VH_ONS_AITargetH_Pawn_L_BP_C":     {"CommandShip", "Command Ship"},
		"VH_ONS_AITargetL_Pawn_BP_C":       {"FighterSwarm", "Fighter Swarm"},
		"VH_ONS_Unknown_Pawn_BP_C":         {"", "ONS Unknown"},
	} {
		if k, l := viewerShipKind(class); k != want[0] || l != want[1] {
			t.Errorf("viewerShipKind(%q) = %q, %q; want %q", class, k, l, want)
		}
	}
}

func TestParseViewerTerrain(t *testing.T) {
	tr := parseViewerTerrain(strings.NewReader("grid 3 2 -100 -50 10.5 20\n1 2 n\nn 5 -6\n"))
	if tr == nil || tr.NX != 3 || tr.NY != 2 || tr.DX != 10.5 || len(tr.Z) != 6 || tr.Z[2] != nil || *tr.Z[5] != -6 {
		t.Fatalf("got %+v", tr)
	}
	if parseViewerTerrain(strings.NewReader("grid 3 2 0 0 1 1\n1 2 3\n")) != nil {
		t.Fatal("accepted a short grid")
	}
}

func TestViewerMapNamePVEEventRow(t *testing.T) {
	if n := viewerMapName("PVE_S1E4"); n == "" || n == "PVE_S1E4" {
		t.Fatalf("viewerMapName(PVE_S1E4) = %q", n)
	}
}

func TestParseViewerTerrainBoxes(t *testing.T) {
	tr := parseViewerTerrain(strings.NewReader("grid 2 2 0 0 1 1\n1 2\nn 4\nbox warp -10 -20 -30 10 20 30\nbox bad 1 2\n"))
	if tr == nil || len(tr.Boxes) != 1 || tr.Boxes[0].Kind != "warp" || tr.Boxes[0].Lo != [3]float64{-10, -20, -30} || tr.Boxes[0].Hi != [3]float64{10, 20, 30} {
		t.Fatalf("got %+v", tr)
	}
}

func TestParseViewerTerrainLayered(t *testing.T) {
	body := "grid2 2 2 0 0 10 10\nn t5,b-20,o40\nw0,t-30 b7\nbox warp 0 0 0 1 1 1\n"
	tr := parseViewerTerrain(strings.NewReader(body))
	if tr == nil || len(tr.Cells) != 4 || len(tr.Boxes) != 1 {
		t.Fatalf("parse: %+v", tr)
	}
	if tr.Z[0] != nil || *tr.Z[1] != 40 || *tr.Z[2] != 0 || tr.Z[3] != nil {
		t.Fatalf("tops: %v %v %v %v", tr.Z[0], tr.Z[1], tr.Z[2], tr.Z[3])
	}
	if parseViewerTerrain(strings.NewReader("grid2 2 1 0 0 1 1\nn x5\n")) != nil {
		t.Fatal("bad kind letter accepted")
	}
}

// The battle chip counts a match's roster, not the ships whose pawns exist:
// the host's snapshot only carries a player once spawned, so "Conquest ·
// Skybridge 0 players" showed while a player was loaded in and flying
// (operator report 2026-10-17).
func TestViewerBattlesCountsTheRosterNotOnlySpawnedPawns(t *testing.T) {
	database := useTempMmogPlayerStateDB(t)
	if _, err := database.Exec(`INSERT INTO matches(id,battle_match_id,game_mode,map,status,fleet_type)
		VALUES('m1','bm-loaded','TDM','Glacier','active',1)`); err != nil {
		t.Fatalf("seed match: %v", err)
	}
	if _, err := database.Exec(`INSERT INTO match_slots(match_id,user_id,team)
		VALUES('m1','650dd79476a1484b8adcd01ac2f17354',1)`); err != nil {
		t.Fatalf("seed slot: %v", err)
	}

	viewerState.mu.Lock()
	viewerState.snaps = map[string]viewerSnapshot{
		// Loaded in, nothing spawned yet: the case that read 0.
		"bm-loaded": {at: time.Now(), ships: []viewerShip{{Class: "VH_ONS_AITargetM_Pawn_BP_C", NPC: true}}},
		// No match row: the roster cannot answer, the ship count must.
		"bm-direct": {at: time.Now(), ships: []viewerShip{{Class: "VH_AssaultM_Pawn_T2_BP_C"}}},
	}
	viewerState.mu.Unlock()
	t.Cleanup(func() {
		viewerState.mu.Lock()
		delete(viewerState.snaps, "bm-loaded")
		delete(viewerState.snaps, "bm-direct")
		viewerState.mu.Unlock()
	})

	got := map[string]int{}
	for _, b := range viewerBattles(false) {
		got[b.BattleMatchID] = b.Players
	}
	if got["bm-loaded"] != 1 {
		t.Errorf("bm-loaded players = %d, want 1 from the roster", got["bm-loaded"])
	}
	if got["bm-direct"] != 1 {
		t.Errorf("bm-direct players = %d, want 1 from the ships", got["bm-direct"])
	}
}

func TestParseViewerSnapshotYawAndProjectiles(t *testing.T) {
	ships, proj, _, ids := parseViewerSnapshot(strings.NewReader("1 2 3 1 80 0 - VH_Assault_C 90 17\n4 5 6 2 50 1 - VH_Scout_C\np 7 8 9\np x 1 2\n"))
	if len(ships) != 2 || ships[0].Yaw == nil || *ships[0].Yaw != 90 || ships[1].Yaw != nil || ships[0].VID != 17 || ships[1].VID != 0 {
		t.Fatalf("ships: %+v", ships)
	}
	if len(proj) != 1 || proj[0] != [3]float64{7, 8, 9} || ids != nil {
		t.Fatalf("proj: %v ids %v", proj, ids)
	}
	ships, _, _, _ = parseViewerSnapshot(strings.NewReader("1 2 3 2 80 1 - VH_AssaultM_AI_Pawn_T1_BP_C 90 17 Iron%20Maw\n1 2 3 2 80 1 - VH_Scout_C 90 18 -\n"))
	if len(ships) != 2 || ships[0].label != "Iron Maw" || ships[1].label != "" {
		t.Fatalf("names: %+v", ships)
	}
	_, proj, kinds, ids := parseViewerSnapshot(strings.NewReader("p 1 2 3 m 4711\np 4 5 6 b 12\n"))
	if len(proj) != 2 || kinds != "mb" || len(ids) != 2 || ids[0] != 4711 || ids[1] != 12 {
		t.Fatalf("ids: proj %v kinds %q ids %v", proj, kinds, ids)
	}
}
