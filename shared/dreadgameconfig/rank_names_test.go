package dreadgameconfig

import "testing"

// The rank ladder is the client's own (data/datatables/Progression/Ranks/
// DN_Ranks_Player.json), and every name in it is real: the dump wrote "[text]"
// until scripts/gen-rank-names.py read the asset's FText literals.
func TestRankTitlesComeFromTheClientsRankTable(t *testing.T) {
	names := RankNames()
	if len(names) != 51 {
		t.Fatalf("%d rank names, the table has 51 rows", len(names))
	}
	for i, n := range names {
		if n == "" || n == "[text]" {
			t.Errorf("rank %d has no name (%q)", i+1, n)
		}
	}
	// The ends of the ladder, and one from the middle: the names are in row
	// order, so a mis-ordered or off-by-one read shows up here.
	for rank, want := range map[int32]string{1: "Fledgling", 2: "Warchild", 30: "Bannerman V", 31: "Ship Master I", 51: "Anax of the Belt"} {
		if got := RankTitle(rank); got != want {
			t.Errorf("rank %d title = %q, want %q", rank, got, want)
		}
	}
	// Out of range: nothing, never a wrapped-around or invented title.
	for _, rank := range []int32{0, -1, 52, 1000} {
		if got := RankTitle(rank); got != "" {
			t.Errorf("rank %d title = %q, want none", rank, got)
		}
	}
}
