package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func fleetDo(t *testing.T, pid, method, body string) (int, map[string]json.RawMessage) {
	t.Helper()
	var req *http.Request
	rec := httptest.NewRecorder()
	if method == http.MethodGet {
		req = httptest.NewRequest(method, "/launcher/fleet", nil)
		req.Header.Set("X-User-ID", pid)
		launcherFleetHandler(rec, req)
	} else {
		req = httptest.NewRequest(method, "/launcher/fleet/action", strings.NewReader(body))
		req.Header.Set("X-User-ID", pid)
		launcherFleetAction(rec, req)
	}
	out := map[string]json.RawMessage{}
	_ = json.Unmarshal(rec.Body.Bytes(), &out)
	return rec.Code, out
}

// The library round trip: LOADOUT A saved, loaded into B, B then equal to A;
// replace/rename/delete; a fit for one ship cannot go into another.
func TestFleetLibrary(t *testing.T) {
	database := useTempMmogPlayerStateDB(t)
	t.Setenv("DN_LOADOUT_B", "1")
	const pid = "650dd79476a1484b8adcd01ac2f17354"
	if err := seedMmogPlayerState(database, pid); err != nil {
		t.Fatal(err)
	}
	a := agostaLoadout(t, pid)
	ship := a.loadoutID()
	// Make A differ from B's default so the copy shows.
	if _, err := database.Exec(`UPDATE player_ship_loadouts SET perk_com_id=-1, ability_internal_id=-1 WHERE user_id=? AND loadout_id=?`, pid, ship); err != nil {
		t.Fatal(err)
	}

	code, d := fleetDo(t, pid, http.MethodGet, "")
	if code != 200 || len(d["ships"]) < 10 || string(d["online"]) != "false" {
		t.Fatalf("GET: %d %s", code, d["error"])
	}
	var items map[string]fleetItemView
	_ = json.Unmarshal(d["items"], &items)
	if len(items) == 0 {
		t.Error("no item descriptions")
	}

	code, d = fleetDo(t, pid, http.MethodPost, `{"op":"save","ship":`+itoa(ship)+`,"variant":0,"name":"brawl"}`)
	var saved []fleetSaved
	_ = json.Unmarshal(d["saved"], &saved)
	if code != 200 || len(saved) != 1 || saved[0].Name != "brawl" || saved[0].Ship != ship || saved[0].Fit.Perks[0] != -1 {
		t.Fatalf("save: %d %s %+v", code, d["error"], saved)
	}
	id := itoa64(saved[0].ID)

	if code, d = fleetDo(t, pid, http.MethodPost, `{"op":"apply","id":`+id+`,"variant":1}`); code != 200 || string(d["ship"]) != itoa(ship) || len(d["loadouts"]) < 10 {
		t.Fatalf("apply: %d %s ship=%s", code, d["error"], d["ship"])
	}
	b, ok := fleetFindLoadout(pid, ship, 1)
	if !ok || fitOf(b) != saved[0].Fit {
		t.Fatalf("B after apply: %+v want %+v", fitOf(b), saved[0].Fit)
	}

	// Another ship's loadout cannot overwrite this entry.
	var other int32
	for _, l := range fleetPlayerLoadouts(pid) {
		if l.loadoutID() != ship {
			other = l.loadoutID()
		}
	}
	if code, _ = fleetDo(t, pid, http.MethodPost, `{"op":"replace","id":`+id+`,"ship":`+itoa(other)+`,"variant":0}`); code != 400 {
		t.Errorf("replace from another ship: %d", code)
	}
	if code, _ = fleetDo(t, pid, http.MethodPost, `{"op":"rename","id":`+id+`,"name":"snipe"}`); code != 200 {
		t.Errorf("rename: %d", code)
	}
	if code, _ = fleetDo(t, pid, http.MethodPost, `{"op":"apply","id":999,"variant":0}`); code != 404 {
		t.Errorf("apply missing: %d", code)
	}
	// Another player cannot touch it.
	if code, _ = fleetDo(t, "00000000000000000000000000000001", http.MethodPost, `{"op":"apply","id":`+id+`,"variant":0}`); code == 200 {
		t.Error("another player applied the entry")
	}
	code, d = fleetDo(t, pid, http.MethodPost, `{"op":"delete","id":`+id+`}`)
	saved = nil
	_ = json.Unmarshal(d["saved"], &saved)
	if code != 200 || len(saved) != 0 {
		t.Errorf("delete: %d %+v", code, saved)
	}
}

// /clone: a player's loadout as a boss -- an AI base of the hull line, the
// fit's modules/officers by path, its look from the appearance string.
func TestCloneBossDef(t *testing.T) {
	database := useTempMmogPlayerStateDB(t)
	const pid = "650dd79476a1484b8adcd01ac2f17354"
	if err := seedMmogPlayerState(database, pid); err != nil {
		t.Fatal(err)
	}
	for _, l := range ownedShipLoadoutsForPlayerData(mmogPlayerStateForPID(pid), pid) {
		d, err := cloneBossDef("Somebody", l)
		if err != nil {
			t.Errorf("%s: %v", l.loadoutName, err)
			continue
		}
		if d.Name != "Evil Somebody" || d.Base == "" || d.Class == "" || d.Tier == "" {
			t.Errorf("%s: %+v", l.loadoutName, d)
		}
	}
	if _, ok := bossByID(cloneIDBase - 1); ok {
		t.Log("a saved boss at 9999?")
	}
}

func itoa(v int32) string   { return itoa64(int64(v)) }
func itoa64(v int64) string { b, _ := json.Marshal(v); return string(b) }

// /clone me: queues bossdef <clone id> <n>, and the host's fetch of that id
// returns "Evil <name>" on an AI base.
func TestCloneChat(t *testing.T) {
	database := useTempMmogPlayerStateDB(t)
	admin := "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4"
	t.Setenv("DN_ADMINS", admin)
	if err := seedMmogPlayerState(database, admin); err != nil {
		t.Fatal(err)
	}
	base := queueHostCommand("other-match", "noop x")
	out, _ := handleChatCommand(admin, "dreadnought.global", "/clone me 2")
	got := hostCommandsSince("m1", base)
	if len(got) != 1 || !strings.Contains(got[0], " bossdef 1000") || !strings.HasSuffix(got[0], " 2") {
		t.Fatalf("reply %q, queued %q", out, got)
	}
	id := strings.Fields(got[0])[len(strings.Fields(got[0]))-2]
	req := httptest.NewRequest(http.MethodGet, "/battle/boss?id="+id, nil)
	req.RemoteAddr = "127.0.0.1:5555"
	rec := httptest.NewRecorder()
	battleBossHandler(rec, req)
	if body := rec.Body.String(); !strings.Contains(body, "name=Evil ") || !strings.Contains(body, "_AI_") && !strings.Contains(body, "ship=") {
		t.Errorf("host text:\n%s", body)
	} else {
		t.Logf("%s\n%s", out, body)
	}
	for _, bad := range []string{"/clone", "/clone nobody", "/clone me 11"} {
		base := queueHostCommand("other-match", "noop x")
		out, _ := handleChatCommand(admin, "dreadnought.global", bad)
		if got := hostCommandsSince("m1", base); len(got) != 0 {
			t.Errorf("%s queued %q (reply %q)", bad, got, out)
		}
	}
}
