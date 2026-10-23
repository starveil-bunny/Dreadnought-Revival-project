package main

import (
	"fmt"
	"net"
	"net/http"
	"strconv"
	"strings"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/protocol"
)

// Match chat rooms. The battle HUD's All/Team chat goes through mmogbrain like
// the hangar's (UYMmogChat EChatType::MatchAll / MatchTeam), into the channel
// named in the client's MatchAll/MatchTeam slot; with the slot empty every
// line fails with "SendChat to MatchAll failed: channel name is empty". As
// with Global, only a join notice naming the player fills a slot (see
// joinChannelAnnounced), and the type is the name's last segment: "all" and
// "team" are two of the six the classifier accepts (chatChannelType).
//
// Rooms are "<matches.id>.all" and "<matches.id>-<team>.team", joined by
// socialHub.joinMatchChannels when the travel push (YA_Connect) goes out.
// Verified live 2026-10-15 (Havoc, headless client): both join notices arrive,
// but Enter in a match logs "HUDWidgetManager doesn't support
// AYHUD::OpenChatAllWindow yet" -- the shipped client has no in-match chat
// window, so nothing can be typed there. Commands work from the hangar chat
// (in a match, a running match is targeted via activeMatchSlot).

func matchAllChannel(matchID string) string { return matchID + ".all" }
func matchTeamChannel(matchID string, team int) string {
	return fmt.Sprintf("%s-%d.team", matchID, team) // same as joinMatchChannels
}

// matchIDFromChannel is the matches.id of a match room, "" for any other.
func matchIDFromChannel(channel string) string {
	switch {
	case strings.HasSuffix(channel, ".all"):
		return strings.TrimSuffix(channel, ".all")
	case strings.HasSuffix(channel, ".team"):
		id := strings.TrimSuffix(channel, ".team")
		if i := strings.LastIndex(id, "-"); i > 0 {
			if _, err := strconv.Atoi(id[i+1:]); err == nil {
				return id[:i]
			}
		}
	}
	return ""
}

func battleMatchIDFor(matchID string) string {
	database := currentMmogPlayerStateDB()
	if database == nil {
		return ""
	}
	var id string
	_ = database.QueryRow(`SELECT battle_match_id FROM matches WHERE id=?`, matchID).Scan(&id)
	return id
}

// activeMatchSlot is the player's running match and team, if any.
func activeMatchSlot(pid string) (matchID string, team int, ok bool) {
	database := currentMmogPlayerStateDB()
	if database == nil {
		return "", 0, false
	}
	rows, err := database.Query(`SELECT s.match_id, s.user_id, s.team FROM match_slots s
		JOIN matches m ON m.id = s.match_id WHERE m.status = 'active' ORDER BY m.created_at DESC`)
	if err != nil {
		return "", 0, false
	}
	defer func() { _ = rows.Close() }()
	for rows.Next() {
		var mid, uid string
		var t int
		if rows.Scan(&mid, &uid, &t) == nil && protocol.NormalizePlayerPID(uid) == pid {
			return mid, t, true
		}
	}
	return "", 0, false
}

// battleCommandsHandler: GET /battle/commands?match=<battle_match_id>&since=<seq>
// (loopback). One "<seq> <command> <arg>" per line; see chat_commands.go.
func battleCommandsHandler(w http.ResponseWriter, r *http.Request) {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		http.Error(w, "loopback only", http.StatusForbidden)
		return
	}
	since, _ := strconv.ParseInt(r.URL.Query().Get("since"), 10, 64)
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	for _, line := range hostCommandsSince(r.URL.Query().Get("match"), since) {
		_, _ = fmt.Fprintln(w, line)
	}
}
