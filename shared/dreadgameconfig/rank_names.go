package dreadgameconfig

import (
	"encoding/json"
	"os"
	"sort"
	"strconv"
	"sync"
)

// The player's rank titles, from the client's own rank table
// (data/datatables/Progression/Ranks/DN_Ranks_Player.json, the dump of
// DreadGame/Content/Generic/DataTables/Progression/Ranks/DN_Ranks_Player).
//
// This is the "title" the client puts on a player: m_rankName is what
// UI_Generic_CaptainRank and UI_Persistent_PlayerCard render as RankTitle, next
// to the rank number and the rank icon (UI_captain_rank_0..50). There is no
// separate equipped-title entity anywhere -- not in the vanity tables (no
// category 55, and nothing named Title among the 1,494 cosmetics), not in the
// client binary (no m_titleID/TitleTable/PersonalTitle property, and no
// YA_*Title* request in any session's protocol trail), and not in mmogbrain's
// schema. So a player's title is the name of the rank they hold.
//
// The ladder's row keys run "0".."50" and its icons are numbered the same way,
// while current_rank is 1-based (db: CHECK (current_rank >= 1), and
// handlers.RankXPThreshold is written for rank 1..51), so rank R is row R-1:
// rank 1 is the first name. Not verified against a live client.
//
// The dump wrote every m_rankName as the literal "[text]" (bpdump does not
// decode FText); scripts/gen-rank-names.py fills them in from the asset's own
// FText literals, and nothing here invents a name: a rank without one gets "".
func RankNames() []string {
	rankNamesOnce.Do(loadRankNames)
	return rankNames
}

var (
	rankNamesOnce sync.Once
	rankNames     []string
)

func loadRankNames() {
	raw, err := os.ReadFile(DataTablePath("Progression/Ranks/DN_Ranks_Player.json"))
	if err != nil {
		return
	}
	var doc struct {
		Rows map[string]struct {
			Name string `json:"m_rankName"`
		} `json:"rows"`
	}
	if json.Unmarshal(raw, &doc) != nil {
		return
	}
	// Row keys are decimal indices, not necessarily in file order.
	keys := make([]int, 0, len(doc.Rows))
	for k := range doc.Rows {
		if n, err := strconv.Atoi(k); err == nil {
			keys = append(keys, n)
		}
	}
	sort.Ints(keys)
	for _, k := range keys {
		name := doc.Rows[strconv.Itoa(k)].Name
		if name == "[text]" { // undecoded: no title rather than a placeholder
			name = ""
		}
		rankNames = append(rankNames, name)
	}
}

// RankTitle is the title of the 1-based rank, "" when there is none (or no
// table). Callers show nothing in that case; see RankNames.
func RankTitle(rank int32) string {
	if rank < 1 || int(rank) > len(RankNames()) {
		return ""
	}
	return RankNames()[rank-1]
}
