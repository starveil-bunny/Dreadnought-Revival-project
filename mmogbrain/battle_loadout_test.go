package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// The game's loadout record is positional (0x34E550 copies 2 weapon, 4 ability
// and 4 perk slots verbatim), so empty slots must stay in place as 0.
func TestBattleLoadoutKeepsEmptySlotsInPlace(t *testing.T) {
	loadout := mmogShipLoadoutSeed{
		precastLoadoutID:  33489267,
		loadoutName:       "Dover",
		weaponPrimaryID:   84017210,
		weaponSecondaryID: 84017966,
		abilityIDs:        [4]int32{67240123, 0, 83820699, 83820692},
	}
	out := formatBattleLoadout("Default__VH_SniperLight_T2_PrecastLoadout_BP_C", "p", loadout)
	for _, want := range []string{
		"id=Default__VH_SniperLight_T2_PrecastLoadout_BP_C\n",
		"precast=33489267\n",
		"name=Dover\n",
		"abilities=67240123,0,83820699,83820692\n",
		"perks=0,0,0,0\n",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
}

func TestBattleLoadoutRefusesNonLoopback(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/battle/loadout?pid=a&id=b", nil)
	req.RemoteAddr = "10.0.0.26:5000"
	rec := httptest.NewRecorder()
	battleLoadoutHandler(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Fatalf("status %d, want 403 for a non-loopback caller", rec.Code)
	}
}

func TestBattlePlayerNameLookup(t *testing.T) {
	database := useTempMmogPlayerStateDB(t)
	const pid = "0123456789abcdef0123456789abcdef"
	if err := seedMmogPlayerState(database, pid); err != nil {
		t.Fatal(err)
	}
	if _, err := database.Exec(`UPDATE player_state SET display_name='Tester' WHERE user_id=?`, pid); err != nil {
		t.Fatal(err)
	}
	req := httptest.NewRequest(http.MethodGet, "/battle/player?pid="+pid, nil)
	req.RemoteAddr = "127.0.0.1:5000"
	rec := httptest.NewRecorder()
	battlePlayerHandler(rec, req)
	// captain= added 2026-10-14: the host copies it into the player state's
	// m_captainDisplayInformation (otherwise every captain draws as default).
	if want := "name=Tester\ncaptain=" + defaultCaptainDisplayInfo + "\n"; rec.Code != http.StatusOK || rec.Body.String() != want {
		t.Fatalf("got %d %q", rec.Code, rec.Body.String())
	}
	req = httptest.NewRequest(http.MethodGet, "/battle/player?pid="+pid, nil)
	req.RemoteAddr = "10.0.0.5:5000"
	rec = httptest.NewRecorder()
	battlePlayerHandler(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Fatalf("non-loopback: %d", rec.Code)
	}
}

// The default-loadout endpoint exists for the one case the client never sends
// an id: a player who picked nothing at orbit (StartMatch with no pawn logs and
// returns, so nothing respawns them). The property the host depends on is that
// the id returned is resolvable through the SAME path the clicked id would take
// (/battle/loadout), because it goes on to ServerSpawnNearActor.
func TestBattleDefaultLoadoutResolvesAsAPickedID(t *testing.T) {
	database := useTempMmogPlayerStateDB(t)
	const pid = "0123456789abcdef0123456789abcdef"
	if err := seedMmogPlayerState(database, pid); err != nil {
		t.Fatal(err)
	}
	req := httptest.NewRequest(http.MethodGet, "/battle/default-loadout?pid="+pid, nil)
	req.RemoteAddr = "127.0.0.1:5000"
	rec := httptest.NewRecorder()
	battleDefaultLoadoutHandler(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d: %s", rec.Code, rec.Body.String())
	}
	var id string
	for _, line := range strings.Split(rec.Body.String(), "\n") {
		if strings.HasPrefix(line, "id=") {
			id = strings.TrimPrefix(line, "id=")
		}
	}
	if id == "" {
		t.Fatalf("no id= in:\n%s", rec.Body.String())
	}
	if _, ok := battleLoadoutFor(pid, id); !ok {
		t.Fatalf("the default id %q is not one /battle/loadout resolves", id)
	}
	// A fresh account's flagship is its first fleet entry
	// (buildConfigBackedStarterFleet sets flagship from loadouts[0]).
	state := mmogPlayerStateForPID(pid)
	want := withLoadoutVariants(pid, ownedShipLoadoutsForPlayerData(state, pid))[0].entryID()
	if id != want {
		t.Errorf("default id %q, want the flagship %q", id, want)
	}

	req = httptest.NewRequest(http.MethodGet, "/battle/default-loadout?pid="+pid, nil)
	req.RemoteAddr = "10.0.0.5:5000"
	rec = httptest.NewRecorder()
	battleDefaultLoadoutHandler(rec, req)
	if rec.Code != http.StatusForbidden {
		t.Errorf("non-loopback: %d", rec.Code)
	}
	req = httptest.NewRequest(http.MethodGet, "/battle/default-loadout", nil)
	req.RemoteAddr = "127.0.0.1:5000"
	rec = httptest.NewRecorder()
	battleDefaultLoadoutHandler(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Errorf("no pid: %d", rec.Code)
	}
}
