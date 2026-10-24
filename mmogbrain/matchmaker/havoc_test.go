package matchmaker

import "testing"

// The client queues its Havoc tile as "HAVOC"; it must reach the Havoc host
// mode on a Havoc map, not Onslaught (the old alias). From PR #80.
func TestHavocRouting(t *testing.T) {
	for _, in := range []string{"HAVOC", "PvE_Havoc"} {
		if !ValidGameMode(in) {
			t.Fatalf("%s not accepted", in)
		}
		if got := NormalizeGameMode(in); got != HavocGameMode {
			t.Fatalf("%s -> %s, want %s", in, got, HavocGameMode)
		}
	}
	if maps := mapsByGameMode[HavocGameMode]; len(maps) != 2 {
		t.Fatalf("havoc maps = %v", maps)
	}
	if matchTeam(HavocGameMode, 1) != 1 {
		t.Fatal("havoc is co-operative: every player on team 1")
	}
	if got := runnableGameMode(HavocGameMode); got != HavocGameMode {
		t.Fatalf("havoc runs as %s", got)
	}
	t.Setenv("DN_HAVOC", "")
	for _, c := range GameModeConfigs() {
		if c.Name == havocClientName {
			t.Fatal("HAVOC listed without DN_HAVOC=1")
		}
	}
	t.Setenv("DN_HAVOC", "1")
	found := false
	for _, c := range GameModeConfigs() {
		found = found || c.Name == havocClientName
	}
	if !found {
		t.Fatal("HAVOC not listed with DN_HAVOC=1")
	}
}

func TestRotationSwitches(t *testing.T) {
	t.Cleanup(func() { SetRotation(nil, nil) })
	SetRotation(map[string]bool{"Gorge": false, "Skybridge": false}, map[string]bool{"TER": false, "Havoc": true})
	if ValidGameMode("Territory") || ValidGameMode("TER") {
		t.Error("switched-off Conquest still queues")
	}
	t.Setenv("DN_HAVOC", "")
	listed := map[string]bool{}
	for _, c := range GameModeConfigs() {
		listed[c.Name] = true
	}
	if listed["TER"] || listed["Territory"] || !listed[havocClientName] || !listed["TDM"] {
		t.Errorf("listed %v", listed)
	}
	for i := 0; i < 20; i++ {
		if m := rotationMaps(conquestMaps); len(m) != 1 || m[0].Name != "Highlands" {
			t.Fatalf("conquest maps %v", m)
		}
	}
	SetRotation(map[string]bool{"Highlands": false}, nil)
	if m := rotationMaps(mapsByGameMode["TM"]); len(m) != 1 {
		t.Error("all-off list must stay whole")
	}
	if len(RotationMapList()) < 15 || len(RotationModeList()) < 8 {
		t.Errorf("lists: %d maps, %v", len(RotationMapList()), RotationModeList())
	}
}
