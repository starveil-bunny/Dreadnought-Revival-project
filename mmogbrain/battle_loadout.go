package main

import (
	"fmt"
	"net"
	"net/http"
	"strings"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/protocol"
)

// battleLoadoutHandler serves one player's loadout to the battle-server mod.
//
// Why it exists (verified 2026-09-25 from the exe): the only loadout message a
// client sends a battle server is ServerPlayerClickedShipLoadout(FName id); the
// server's mmog AutoLogin (0x2AABCB0) is a stub in this exe, and its fleet
// manager (UpdatePlayerFleetData, 0x35FDF0) only ever reads the host's OWN
// account. So a host that should spawn a player's custom fit has no way to
// learn it -- the original servers were a separate server build. The mod
// learns whose connection it is from the ?DNPID= option mmogbrain adds to the
// YA_Connect travel address (read back from UNetConnection+0x198 on the host)
// and asks here for the loadout the client picked.
//
// The answer is the same record the client builds its own loadout from
// (YA_PlayerFleets -> 0x34E550 -> FYShipImportLoadoutInfo): precast id, name,
// ship class, display info, 2 weapon slots, 4 ability slots, 4 perk slots --
// positional, zeros included, because the game's record is positional.
//
// GET /battle/loadout?pid=<pid>&id=<loadout id as the client sent it>
// Loopback only: the battle servers run on this host, and nothing here needs a
// secret passed through Wine. Plain "key=value" lines so the mod parses it
// without a JSON library.
func battleLoadoutHandler(w http.ResponseWriter, r *http.Request) {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		http.Error(w, "loopback only", http.StatusForbidden)
		return
	}
	pid := normalizedPlayerStatePID(r.URL.Query().Get("pid"))
	id := r.URL.Query().Get("id")
	if pid == "" || id == "" {
		http.Error(w, "pid and id are required", http.StatusBadRequest)
		return
	}
	loadout, ok := battleLoadoutFor(pid, id)
	if !ok {
		http.Error(w, "no such loadout for this player", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = w.Write([]byte(formatBattleLoadout(id, pid, loadout)))
}

// battleDefaultLoadoutHandler answers GET /battle/default-loadout?pid= with the
// loadout the player's own ship-select screen has as its flagship, for the host
// to spawn when a match starts with the player having picked nothing.
//
// Why: at orbit the client sends ServerPlayerClickedShipLoadout(FName id) only
// when a card is clicked; the stock client has no auto-select on the selection
// timer's expiry (UI_HUD_ShipSelect_WBP only sets local widget state, and the
// countdown widget is display-only), and AYPlayerController::StartMatch with no
// pawn (0x59AD60) logs and returns without rescheduling. So a player who picks
// nothing is a black screen forever unless the host chooses for them. This
// answers "which one" with the game's own answer rather than a guess.
//
// GET /battle/default-loadout?pid=<pid>
// Loopback only, and the same "key=value" shape as /battle/loadout -- the same
// keys in the same order -- so the mod's existing parser works untouched. The
// pid is required and is not defaulted: see the comment in the handler.
func battleDefaultLoadoutHandler(w http.ResponseWriter, r *http.Request) {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		http.Error(w, "loopback only", http.StatusForbidden)
		return
	}
	// Deliberately NOT normalizedPlayerStatePID: that falls back to
	// defaultMmogPlayerPID, which would answer with the default account's
	// flagship when the host's ?DNPID= lookup came up empty -- the exact failure
	// this endpoint exists to cover (PlayerPIDForManager failing at orbit time),
	// and the wrong player's fit is worse here than no answer. A pid the client
	// normalizes differently is the host's problem to solve, not ours.
	pid := protocol.NormalizePlayerPID(r.URL.Query().Get("pid"))
	if pid == "" {
		http.Error(w, "pid is required", http.StatusBadRequest)
		return
	}
	id, loadout, ok := battleDefaultLoadoutFor(pid)
	if !ok {
		http.Error(w, "no loadout for this player", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = w.Write([]byte(formatBattleLoadout(id, pid, loadout)))
}

// battleDefaultLoadoutFor is the player's flagship: the fleet their state has
// active (mmogPlayerState.activeFleet) and, inside it, the entry its own
// FlagShipLoadoutIndex points at -- the ship the client's orbit screen opens
// on, and the one it would have sent had the player picked. The stored flagship
// id goes stale when ships are added or removed after it was set, so a flagship
// that matches nothing falls back to the fleet's first entry, which is what
// buildConfigBackedStarterFleet calls the flagship for a fresh account; an empty
// list has no answer at all.
//
// The id returned is entryID(): the "ID" field of the client's own
// YA_PlayerFleets entry, the very string ServerPlayerClickedShipLoadout carries
// and the one /battle/loadout resolves, so the host can pass it to
// ServerSpawnNearActor unchanged.
func battleDefaultLoadoutFor(pid string) (string, mmogShipLoadoutSeed, bool) {
	state := mmogPlayerStateForPID(pid)
	loadouts := withLoadoutVariants(pid, ownedShipLoadoutsForPlayerData(state, pid))
	if len(loadouts) == 0 {
		return "", mmogShipLoadoutSeed{}, false
	}
	fleet := state.activeFleet()
	if fleet.flagshipLoadoutID != 0 || fleet.flagshipShipID != 0 {
		for _, loadout := range loadouts {
			if loadout.loadoutID() == fleet.flagshipLoadoutID || loadout.precastLoadoutID == fleet.flagshipShipID {
				return loadout.entryID(), loadout, true
			}
		}
	}
	return loadouts[0].entryID(), loadouts[0], true
}

// battleLoadoutFor finds the player's loadout whose client-facing id (the
// "ID" field of the YA_PlayerFleets entry) is id -- the list the client itself
// received, so both sides resolve the same row.
func battleLoadoutFor(pid, id string) (mmogShipLoadoutSeed, bool) {
	for _, loadout := range withLoadoutVariants(pid, ownedShipLoadoutsForPlayerData(mmogPlayerStateForPID(pid), pid)) {
		if loadout.entryID() == id {
			return loadout, true
		}
	}
	return mmogShipLoadoutSeed{}, false
}

func formatBattleLoadout(id, pid string, loadout mmogShipLoadoutSeed) string {
	ints := func(vs ...int32) string {
		parts := make([]string, len(vs))
		for i, v := range vs {
			parts[i] = fmt.Sprint(v)
		}
		return strings.Join(parts, ",")
	}
	var b strings.Builder
	fmt.Fprintf(&b, "id=%s\n", id)
	fmt.Fprintf(&b, "pid=%s\n", pid)
	fmt.Fprintf(&b, "precast=%d\n", loadout.precastLoadoutID)
	fmt.Fprintf(&b, "name=%s\n", loadout.loadoutName)
	fmt.Fprintf(&b, "class=%d\n", loadoutEYShipClass(loadout))
	fmt.Fprintf(&b, "display=%s\n", loadout.displayInfo())
	fmt.Fprintf(&b, "weapons=%s\n", ints(loadout.weaponPrimaryItemID(), loadout.weaponSecondaryItemID()))
	fmt.Fprintf(&b, "abilities=%s\n", ints(loadout.abilityItemID(0), loadout.abilityItemID(1), loadout.abilityItemID(2), loadout.abilityItemID(3)))
	fmt.Fprintf(&b, "perks=%s\n", ints(loadout.perkItemID(0), loadout.perkItemID(1), loadout.perkItemID(2), loadout.perkItemID(3)))
	return b.String()
}

// battlePlayerHandler answers GET /battle/player?pid= with "name=<display
// name>": the name battle-server-mod gives a joining player. Added 2026-09-28:
// the host can only name players from their join URL, whose Name= the client
// empties (UYGameEngine::Browse) and whose PlayerName= (dn-launcher's start
// token) survives only the first match after the game starts -- a later
// 3-player proving ground joined without it and everyone was a number again.
// Loopback only, like /battle/loadout.
func battlePlayerHandler(w http.ResponseWriter, r *http.Request) {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		http.Error(w, "loopback only", http.StatusForbidden)
		return
	}
	pid := protocol.NormalizePlayerPID(r.URL.Query().Get("pid"))
	if pid == "" {
		http.Error(w, "pid is required", http.StatusBadRequest)
		return
	}
	state := mmogPlayerStateForPID(pid)
	name := strings.TrimSpace(state.displayName)
	if name == "" || name == "Local" {
		http.Error(w, "no name for this player", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = fmt.Fprintf(w, "name=%s\n", name)
	// captain= is the saved captain appearance (YA_SavePlayerDisplayInformation
	// "disp", same string YA_GetPlayersInformation returns as DisplayInfo). The
	// host copies it into AYPlayerReplicationInfo::m_captainDisplayInformation
	// (replicated FString +0x758, RepNotify OnRep_CaptainData), which a
	// login-less battle server otherwise leaves empty -> default captains.
	if captain := strings.TrimSpace(state.displayInfo); captain != "" && !strings.ContainsAny(captain, "\r\n") {
		_, _ = fmt.Fprintf(w, "captain=%s\n", captain)
	}
}
