package main

import (
	"fmt"
	"io"
	"math/rand"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/matchmaker"
	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/protocol"
	dreadconfig "github.com/darkace1998/Dreadnought-Revival-project/shared/dreadgameconfig"
	"github.com/sirupsen/logrus"
)

// Chat commands: a chat line starting with "/" or "!" is a command, answered
// by the "Server" sender to the typist only and never relayed to the room.
// Works in every room the client sends through mmogbrain -- the hangar's
// Global/language rooms and the match rooms (match_chat.go).
//
// Most settings a command changes are env-driven and read at use, so a
// command sets the variable for this process (lost on restart). Commands that
// act inside a match are queued for the battle hosts (GET /battle/commands,
// polled by battle-server-mod): from a match room they reach that match, from
// a hangar room every running match.
//
// DN_ADMINS: comma-separated player ids allowed the admin commands.
// DN_CHAT_COMMANDS=0 turns commands off (lines are relayed as plain chat).
// DN_CHAT_HELP_ON_LOGIN=0 stops the /help text sent at login.

func chatCommandsEnabled() bool { return os.Getenv("DN_CHAT_COMMANDS") != "0" }

func isChatAdmin(pid string) bool {
	if pid == operatorPID {
		return true
	}
	pid = protocol.NormalizePlayerPID(pid)
	if pid == "" {
		return false
	}
	for _, a := range strings.Split(os.Getenv("DN_ADMINS"), ",") {
		if protocol.NormalizePlayerPID(strings.TrimSpace(a)) == pid {
			return true
		}
	}
	return false
}

type chatCommand struct {
	name, args, help string
	admin            bool
	run              func(c *chatCommandCtx) string
}

type chatCommandCtx struct {
	pid, channel string
	args         []string
	// match is the matches.id of the match room the command was typed in,
	// "" from a hangar room.
	match string
}

var chatCommands []chatCommand

func init() {
	chatCommands = []chatCommand{
		{name: "help", args: "[word]", help: "this list, or only lines with word", run: cmdHelp},
		{name: "online", help: "who is online, in battle, searching", run: cmdOnline},
		{name: "season", args: "[1|3|debug|off]", help: "show or set the PvE season", run: cmdSeason},
		{name: "event", args: "[invader|off|<item ids>]", help: "show or set the event cosmetics grant", run: cmdEvent},
		{name: "xp", args: "[multiplier]", help: "show or set the match XP multiplier", run: cmdXP},
		{name: "match", args: "<havoc|tdm|ffa|pod|turbo|onslaught|conquest|te|pve|pg> [map] | off", help: "pin the next match's mode and map (ffa: TDM, everyone on their own team, no bots)", admin: true, run: cmdMatch},
		// /bunny is off: the client font has no glyphs for much of the ASCII art
		// (2026-10-17). cmdBunny is kept for when the art is redrawn.
		// {name: "bunny", help: "post a bunny to this room", run: cmdBunny},
		{name: "say", args: "<text>", help: "post a Server line to Global", admin: true, run: cmdSay},
		{name: "turbo", args: "on|off", help: "Turbo modifiers (speed, damage, fire rate) on all ships", admin: true, run: hostCmd("turbo", "on", "off")},
		{name: "npc", args: "normal|hard|brutal", help: "NPC tier: extra hull, armor and damage on NPC ships", admin: true, run: hostCmd("npc", "normal", "hard", "brutal")},
		{name: "bots", args: "1|2|3|4", help: "enemy multiplier, 1 = normal: Havoc copies each new enemy N times, Onslaught keeps N times the enemy fighters and assault ships", admin: true, run: hostCmd("bots", "1", "2", "3", "4")},
		{name: "enemy", args: "<ShipId>|off|list", help: "Havoc: every new enemy spawns as this ship (/enemy list)", admin: true, run: cmdEnemy},
		{name: "spawn", args: "<ShipId>|boss [1-8] [x2-x10]", help: "spawn enemies now (Onslaught: assault ships, ShipId ignored; TDM/PodTDM/TER: enemy bots of that ship, the nearest tier the mode has); xN = elite with N times the health", admin: true, run: cmdSpawn},
		{name: "boss", args: "[<id>|list|ShipId|random|stock] [2-10]", help: "one elite boss now: a numbered custom boss from the admin Bosses tab (/boss 1, /boss 1 3 overrides its multiplier); bare /boss picks a random custom boss (stock: a random stock boss at x2); in TDM/PodTDM/TER an enemy bot flying the boss's class at T5", admin: true, run: cmdBoss},
		{name: "clone", args: "<player|me> [ship] [1-10]", help: "an AI copy of a player's ship as an enemy boss, Evil <player>: their flagship, or the named ship of theirs", admin: true, run: cmdClone},
		{name: "team", args: "<player> <1|2>", help: "move a player to team 1 or 2", admin: true, run: cmdTeam},
		{name: "shake", args: "1-10", help: "shake every player's camera (strength 1-10)", admin: true, run: hostCmdCheck("shake", "1-10", func(a string) bool {
			n, err := strconv.Atoi(a)
			return err == nil && n >= 1 && n <= 10
		})},
		{name: "flash", args: "white|red|pink|black|gold|blue", help: "flash every player's screen a colour", admin: true, run: hostCmd("flash", "white", "red", "pink", "black", "gold", "blue")},
		{name: "slowmo", args: "0.2-1", help: "slow the whole match down (1 = normal speed)", admin: true, run: hostCmdCheck("slowmo", "0.2-1", func(a string) bool {
			f, err := strconv.ParseFloat(a, 64)
			return err == nil && f >= 0.2 && f <= 1
		})},
		{name: "kill", args: "me|all|<player>", help: "destroy ships (they respawn)", admin: true, run: cmdKill},
		{name: "meteor", args: "[1-40] [distance] [team]", help: "Onslaught: a Comet per player ship fires n Warp Missile nukes from [distance] uu (default 1, 80000); elsewhere torpedoes rain from overhead (default 12)", admin: true, run: cmdMeteor},
		{name: "swarm", args: "[1-60] [team]", help: "a swarm of Onslaught fighters (default and cap set in the admin Waves tab) near a random player, team 3 (purple, fights everyone) unless given", admin: true, run: cmdSwarm("swarm", 60)},
		{name: "squad", args: "[1-20] [team]", help: "a squad of Onslaught assault ships (default and cap set in the admin Waves tab) near a random player, team 3 unless given", admin: true, run: cmdSwarm("squad", 20)},
		{name: "wave", args: "[x2-x10] | skip|1-50", help: "faux Havoc wave: a boss (random custom boss, else stock) + assault ships + fighters + hunters at once (counts in the admin Waves tab); in Havoc, skip ends the current wave and N makes the next wave wave N", admin: true, run: cmdWave},
		{name: "hunters", args: "[1-12] [team]", help: "a pack of hunters from the curated pool (admin Hunters tab; per player, default in the Waves tab) sent at each player, enemy team unless given", admin: true, run: cmdHunters},
		{name: "fighters", args: "[team]", help: "test (Onslaught): three heralds launch Fighters, Bombers and Interceptors at the player ships", admin: true, run: cmdFighters},
		{name: "props", args: "<object> [filter]", help: "debug: log an object's properties to the mod log (pawn, pawn.<prop>, gm, gs)", admin: true, run: cmdProps},
		{name: "host", args: "<raw command>", help: "debug: send a raw command line to the battle server mod", admin: true, run: cmdHost},
		{name: "nuke", help: "lethal damage to every enemy ship (not command ships)", admin: true, run: cmdNuke},
		{name: "difficulty", args: "recruit|veteran|legendary", help: "Havoc: map difficulty (on Havoc Map1 only legendary swaps a few late-wave enemies)", admin: true, run: hostCmd("difficulty", "recruit", "veteran", "legendary")},
	}
}

// Setting commands are read-only for non-admins: "/season" shows, "/season 3"
// needs DN_ADMINS.
var chatSettingCommands = map[string]bool{"season": true, "event": true, "xp": true}

// handleChatCommand runs body if it is a command. handled=false means relay it
// as ordinary chat.
func handleChatCommand(pid, channel, body string) (reply string, handled bool) {
	if !chatCommandsEnabled() {
		return "", false
	}
	body = strings.TrimSpace(body)
	if len(body) < 2 || (body[0] != '/' && body[0] != '!') {
		return "", false
	}
	fields := strings.Fields(body[1:])
	if len(fields) == 0 {
		return "", false
	}
	name := strings.ToLower(fields[0])
	for i := range chatCommands {
		cmd := &chatCommands[i]
		if cmd.name != name {
			continue
		}
		admin := isChatAdmin(pid)
		if (cmd.admin || (chatSettingCommands[name] && len(fields) > 1)) && !admin {
			return "/" + name + ": admins only.", true
		}
		c := &chatCommandCtx{pid: pid, channel: channel, args: fields[1:], match: matchIDFromChannel(channel)}
		out := cmd.run(c)
		logrus.WithFields(logrus.Fields{"pid": pid, "channel": channel, "admin": admin}).Info("chat command: " + body + " -> " + strings.ReplaceAll(out, "\n", " | "))
		return out, true
	}
	return "Unknown command /" + name + ". /help lists them.", true
}

// chatHelpText is data/chat/help.txt when it exists (edited by hand, read on
// every call so edits apply without a restart), else the generated list.
// File format: "//" lines are notes; lines after "[admin]" are shown to admins
// only, "[all]" switches back.
func chatHelpText(admin bool) string {
	if lines, err := readChatTextFile("help.txt"); err == nil {
		var out []string
		adminOnly := false
		for _, l := range lines {
			switch strings.TrimSpace(l) {
			case "[admin]":
				adminOnly = true
				continue
			case "[all]":
				adminOnly = false
				continue
			}
			if !adminOnly || admin {
				out = append(out, l)
			}
		}
		if t := strings.TrimSpace(strings.Join(out, "\n")); t != "" {
			return t
		}
	}
	return generatedChatHelpText(admin)
}

// generatedChatHelpText lists chatCommands; the seed for help.txt.
func generatedChatHelpText(admin bool) string {
	var b strings.Builder
	b.WriteString("Commands (/ or !):")
	for _, cmd := range chatCommands {
		if cmd.admin && !admin {
			continue
		}
		b.WriteString("\n/" + cmd.name)
		if cmd.args != "" && (admin || !chatSettingCommands[cmd.name]) {
			b.WriteString(" " + cmd.args)
		}
		b.WriteString(" -- " + cmd.help)
	}
	return b.String()
}

// readChatTextFile reads data/chat/<name> minus "//" note lines and trailing
// spaces. A missing file is an error so callers fall back to built-ins.
func readChatTextFile(name string) ([]string, error) {
	raw, err := os.ReadFile(filepath.Join(dreadconfig.DataDir(), "chat", name))
	if err != nil {
		return nil, err
	}
	var lines []string
	for _, l := range strings.Split(strings.ReplaceAll(string(raw), "\r\n", "\n"), "\n") {
		if strings.HasPrefix(l, "//") {
			continue
		}
		lines = append(lines, strings.TrimRight(l, " \t"))
	}
	return lines, nil
}

// cmdHelp: /help is the whole list; /help <word> only the lines containing
// it, since the in-game chat cannot be scrolled back (no binding for it).
func cmdHelp(c *chatCommandCtx) string {
	text := chatHelpText(isChatAdmin(c.pid))
	if len(c.args) == 0 {
		return text
	}
	want := strings.ToLower(strings.TrimLeft(strings.Join(c.args, " "), "/!"))
	var hits []string
	for _, l := range strings.Split(text, "\n") {
		if strings.Contains(strings.ToLower(l), want) {
			hits = append(hits, l)
		}
	}
	if len(hits) == 0 {
		return "No command matches \"" + want + "\"."
	}
	return strings.Join(hits, "\n")
}

func cmdOnline(*chatCommandCtx) string {
	online, inBattle, searching := serverChatCounts()
	return serverChatOnlineLine(online, inBattle, searching, "")
}

var pveSeasonAliases = map[string]string{"1": "PVE_Season1", "3": "PVE_Season3", "debug": "PVE_DebugSeason", "rotate": "rotate", "off": "off"}

func cmdSeason(c *chatCommandCtx) string {
	if len(c.args) == 0 {
		if id := pveSeasonID(); id != "" {
			return "Season: " + id + "."
		}
		return "Season: off."
	}
	id, ok := pveSeasonAliases[strings.ToLower(c.args[0])]
	if !ok {
		return "Unknown season " + c.args[0] + "; one of 1, 3, debug, rotate, off."
	}
	_ = os.Setenv("DN_PVE_SEASON", id)
	return "Season set to " + id + ". Players see it after their next login."
}

// Named event grants: the item ids of each original event's rewards.
var chatEventPresets = map[string]string{
	"invader": "385810495,419365147", // "Invader" coating + Saucer decal (event_grants.go)
}

func cmdEvent(c *chatCommandCtx) string {
	if len(c.args) == 0 {
		if v := os.Getenv("DN_EVENT_GRANTS"); v != "" {
			return "Event grants: " + v + "."
		}
		return "Event grants: off."
	}
	arg := strings.ToLower(c.args[0])
	switch {
	case arg == "off":
		_ = os.Unsetenv("DN_EVENT_GRANTS")
		return "Event grants off."
	case chatEventPresets[arg] != "":
		_ = os.Setenv("DN_EVENT_GRANTS", chatEventPresets[arg])
	default:
		for _, f := range strings.Split(arg, ",") {
			if _, err := strconv.ParseInt(f, 10, 32); err != nil {
				names := make([]string, 0, len(chatEventPresets))
				for k := range chatEventPresets {
					names = append(names, k)
				}
				sort.Strings(names)
				return "Not an item id list or event name (" + strings.Join(names, ", ") + "): " + arg
			}
		}
		_ = os.Setenv("DN_EVENT_GRANTS", arg)
	}
	return "Event grants set to " + os.Getenv("DN_EVENT_GRANTS") + ". Granted at each player's next profile load."
}

// matchXPMultiplier scales every match payout's XP (DN_REWARD_XP_MULT,
// default 1).
func matchXPMultiplier() float64 {
	if v, err := strconv.ParseFloat(strings.TrimSpace(os.Getenv("DN_REWARD_XP_MULT")), 64); err == nil && v >= 0 && v <= 100 {
		return v
	}
	return 1
}

func cmdXP(c *chatCommandCtx) string {
	if len(c.args) == 0 {
		return fmt.Sprintf("XP multiplier: x%g.", matchXPMultiplier())
	}
	v, err := strconv.ParseFloat(strings.TrimPrefix(strings.ToLower(c.args[0]), "x"), 64)
	if err != nil || v < 0 || v > 100 {
		return "XP multiplier must be a number from 0 to 100."
	}
	_ = os.Setenv("DN_REWARD_XP_MULT", strconv.FormatFloat(v, 'g', -1, 64))
	return fmt.Sprintf("XP multiplier set to x%g for matches ending from now on.", v)
}

// Mode names as the matchmaker queues them.
var chatMatchModes = map[string]string{
	"havoc": "Havoc", "tdm": "TDM", "pod": "PodTDM", "turbo": "TurboTDM", "turbotdm": "TurboTDM",
	"onslaught": "Onslaught", "conquest": "TER", "te": "TE", "pve": "PVE", "pg": "BC", "bc": "BC",
}

func cmdMatch(c *chatCommandCtx) string {
	f := strings.TrimSpace(os.Getenv("DN_MATCH_PIN_FILE"))
	if len(c.args) == 0 {
		return "Usage: /match <havoc|tdm|ffa|pod|turbo|onslaught|conquest|te|pve|pg> [map] | off"
	}
	if strings.EqualFold(c.args[0], "off") {
		if f != "" {
			_ = os.Remove(f)
		}
		return "Match pin cleared."
	}
	ffa := strings.EqualFold(c.args[0], "ffa")
	mode, ok := chatMatchModes[strings.ToLower(c.args[0])]
	if ffa {
		mode, ok = "TDM", true
	}
	if !ok {
		mode = c.args[0]
	}
	if f == "" {
		f = "match-pin.json"
		_ = os.Setenv("DN_MATCH_PIN_FILE", f)
	}
	mapName := ""
	if len(c.args) > 1 {
		mapName = c.args[1]
	}
	data := fmt.Sprintf(`{"mode":%q,"map":%q,"ffa":%t}`, mode, mapName, ffa)
	if err := os.WriteFile(f, []byte(data), 0o644); err != nil { //nolint:gosec // local pin file
		return "Could not write the pin: " + err.Error()
	}
	if mapName == "" {
		mapName = "any map"
	}
	if ffa {
		mode = "FFA (TDM, everyone on their own team, no bots)"
	}
	return "Next match pinned: " + mode + " on " + mapName + " (one match)."
}

// The "ffa on" line for a match formed from an ffa pin is queued when the
// match is created, before its host is up, so it outlives hostCommandTTL.
const stickyHostCommandTTL = 30 * time.Minute

func init() {
	matchmaker.OnPinnedFFA = func(battleMatchID string) {
		if battleMatchID != "" {
			queueStickyHostCommand(battleMatchID, "ffa on")
		}
	}
}

func queueStickyHostCommand(battleMatchID, line string) {
	hostCommands.mu.Lock()
	defer hostCommands.mu.Unlock()
	hostCommands.seq++
	hostCommands.list = append(hostCommands.list, queuedHostCommand{seq: hostCommands.seq, match: battleMatchID, line: line, at: time.Now(), sticky: true})
}

func (q queuedHostCommand) live(now time.Time) bool {
	if q.sticky {
		return now.Sub(q.at) < stickyHostCommandTTL
	}
	return now.Sub(q.at) < hostCommandTTL
}

func cmdSay(c *chatCommandCtx) string {
	if len(c.args) == 0 {
		return "Usage: /say <text>"
	}
	socialHubInstance.announce(strings.Join(c.args, " "))
	return "Posted."
}

// --- commands for the battle hosts -----------------------------------------------

type queuedHostCommand struct {
	seq    int64
	match  string // battle_match_id; "" = every host
	line   string
	at     time.Time
	sticky bool // queueStickyHostCommand
}

var hostCommands struct {
	mu   sync.Mutex
	seq  int64
	list []queuedHostCommand
}

// hostCommandTTL: a command not fetched within this long is dropped, so a host
// started later does not replay old commands.
const hostCommandTTL = 60 * time.Second

func queueHostCommand(battleMatchID, line string) int64 {
	hostCommands.mu.Lock()
	defer hostCommands.mu.Unlock()
	now := time.Now()
	kept := hostCommands.list[:0]
	for _, q := range hostCommands.list {
		if q.live(now) {
			kept = append(kept, q)
		}
	}
	hostCommands.seq++
	hostCommands.list = append(kept, queuedHostCommand{seq: hostCommands.seq, match: battleMatchID, line: line, at: now})
	return hostCommands.seq
}

// hostCommandsSince is the commands for battleMatchID after seq, one
// "<seq> <command>" per line.
func hostCommandsSince(battleMatchID string, seq int64) []string {
	hostCommands.mu.Lock()
	defer hostCommands.mu.Unlock()
	var out []string
	for _, q := range hostCommands.list {
		if q.seq > seq && q.live(time.Now()) && (q.match == "" || q.match == battleMatchID) {
			out = append(out, fmt.Sprintf("%d %s", q.seq, q.line))
		}
	}
	return out
}

func hostCmd(name string, allowed ...string) func(c *chatCommandCtx) string {
	return hostCmdCheck(name, strings.Join(allowed, "|"), func(arg string) bool {
		for _, a := range allowed {
			if a == arg {
				return true
			}
		}
		return false
	})
}

// matchCommandTarget is where a match-scoped command goes: the battle of the
// room it was typed in, or every running match when it came from Global.
//
// A match room exists from matchmaking, before its battle does, and
// battleMatchIDFor also comes back empty when the state DB is unavailable. An
// empty target is not "this match": queueHostCommand("") is the broadcast form
// (hostCommandsSince treats "" as any match), so a caller must refuse when ok
// is false rather than deliver the command to every running match behind a
// "sent to this match" reply. Global is the deliberate way to reach them all.
func matchCommandTarget(c *chatCommandCtx) (target, where string, ok bool) {
	if c.match == "" {
		return "", "every running match", true
	}
	if target := battleMatchIDFor(c.match); target != "" {
		return target, "this match", true
	}
	return "", "", false
}

// noLiveBattleReply refuses a match-scoped command typed in a room whose battle
// does not exist yet.
func noLiveBattleReply(command string) string {
	return command + ": no live battle for this match yet -- type it in Global to send it to every running match."
}

// hostCmdCheck queues "<name> <arg>" for the host mod when valid(arg).
func hostCmdCheck(name, usage string, valid func(string) bool) func(c *chatCommandCtx) string {
	return func(c *chatCommandCtx) string {
		if len(c.args) == 0 || !valid(strings.ToLower(c.args[0])) {
			return "Usage: /" + name + " " + usage
		}
		arg := strings.ToLower(c.args[0])
		target, where, ok := matchCommandTarget(c)
		if !ok {
			return noLiveBattleReply("/" + name + " " + arg)
		}
		queueHostCommand(target, name+" "+arg)
		return "/" + name + " " + arg + " sent to " + where + "."
	}
}

// --- login help ------------------------------------------------------------------

// sendChatHelpOnLogin posts the /help text to one player in Global, a few
// seconds after login so the chat window exists.
func sendChatHelpOnLogin(peer *socialPeer) {
	if !chatCommandsEnabled() || !serverChatEnabled() || os.Getenv("DN_CHAT_HELP_ON_LOGIN") == "0" {
		return
	}
	go func() {
		time.Sleep(5 * time.Second)
		if socialHubInstance.peerFor(peer.playerID) != peer {
			return
		}
		sendServerLine(peer, defaultChatChannels[0], chatHelpText(isChatAdmin(peer.playerID)))
	}()
}

// sendServerLine sends one Server line to one player in channel.
func sendServerLine(peer *socialPeer, channel, text string) {
	_ = peer.send(serverChatProfile())
	for _, line := range strings.Split(text, "\n") {
		_ = peer.send(chatMessageNotice("chat.channel.message", channel, serverChatEntry(), line))
	}
}

// answerMmogChatCommand handles a command that arrived as binary YA_Chat. The
// reply goes to the player's social peer: in their match room when a match is
// running, else in global. Returns false when message is not a command.
func answerMmogChatCommand(pid, message string) bool {
	channel := defaultChatChannels[0]
	if matchID, _, ok := activeMatchSlot(pid); ok {
		channel = matchID + ".all"
	}
	reply, ok := handleChatCommand(pid, channel, message)
	if !ok {
		return false
	}
	if peer := socialHubInstance.peerFor(pid); peer != nil && reply != "" {
		sendServerLine(peer, channel, reply)
	}
	return true
}

// operatorPID is the caller of POST /admin/command: the shell on the server
// machine, always admin. Exists because the shipped client has no in-match
// chat window (match_chat.go), so mid-match commands come from `dn cmd`.
const operatorPID = "operator"

// adminCommandHandler: POST /admin/command (loopback), body = one command line
// ("/turbo on"). Runs it as the operator from Global, so host commands go to
// every running match; the reply is the response body.
func adminCommandHandler(w http.ResponseWriter, r *http.Request) {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil || !net.ParseIP(host).IsLoopback() {
		http.Error(w, "loopback only", http.StatusForbidden)
		return
	}
	body, _ := io.ReadAll(io.LimitReader(r.Body, 4096))
	line := strings.TrimSpace(string(body))
	if line != "" && line[0] != '/' && line[0] != '!' {
		line = "/" + line
	}
	reply, ok := handleChatCommand(operatorPID, defaultChatChannels[0], line)
	if !ok {
		http.Error(w, "not a command (or DN_CHAT_COMMANDS=0)", http.StatusBadRequest)
		return
	}
	_, _ = io.WriteString(w, reply+"\n")
}

// havocEnemyIDs are the NPC dataset rows HVC_Map1_P's own waves spawn
// (EnemyNameRecruit/Veteran/Legendary of every PVE_Horde_Manager_Enemy_struct,
// bpdump of the map package, 2026-10-15). /enemy only accepts these, since a
// row the dataset lacks would leave the wave waiting on an enemy that never
// spawns.
var havocEnemyIDs = []string{"AssaultHeavyBoss01", "AssaultHeavyStormT4", "AssaultHeavySwarmTorpT4", "AssaultHeavySwarmTorpT5", "AssaultHeavyT4", "AssaultHeavyT5", "AssaultLightBlastRamT4", "AssaultLightBlastRamT5", "AssaultLightPlasmaRamT4", "AssaultLightPlasmaRamT5", "AssaultLightPurgeRamT4", "AssaultLightPurgeRamT5", "AssaultLightRammerBoss01", "AssaultMediumBarrageT4", "AssaultMediumBarrageT5", "AssaultMediumBasicT2", "AssaultMediumBoss01", "AssaultMediumEasyT1", "AssaultMediumRammerEasyT3", "AssaultMediumRammerT5", "AssaultMediumT3", "AssaultMediumT4", "DreadnoughtHeavyBoss01", "DreadnoughtHeavyNukerT4", "DreadnoughtHeavyNukerT5", "DreadnoughtHeavyPurgeNukerT4", "DreadnoughtHeavyPurgeNukerT5", "DreadnoughtHeavySwarmNukerT4", "DreadnoughtHeavySwarmNukerT5", "DreadnoughtLightBroadsideT3", "DreadnoughtLightMGBroadsideT4", "DreadnoughtLightMGBroadsideT5", "DreadnoughtLightPlasmaBroadsideT5", "DreadnoughtMediumBombersT4", "DreadnoughtMediumBombersT5", "DreadnoughtMediumBoss01", "DreadnoughtMediumEasyT2", "DreadnoughtMediumFightersT4", "DreadnoughtMediumInterceptorsT4", "DreadnoughtMediumInterceptorsT5", "DreadnoughtMediumNukerEasyT2", "DreadnoughtMediumT3", "ScoutHeavyBoss01", "ScoutHeavyEasyT3", "ScoutHeavyT3", "ScoutHeavyT4", "ScoutHeavyT5", "ScoutLightBasicT2", "ScoutLightBasicT3", "ScoutLightT4", "ScoutLightT5", "ScoutMediumT4", "ScoutMediumT5", "SniperHeavyBoss01", "SniperHeavyCatapultT3", "SniperHeavyT4", "SniperHeavyT5", "SniperLightRapidT2", "SniperLightT4", "SniperMediumCatapultT2", "SniperMediumCatapultT3", "SniperMediumEasyT1", "SniperMediumRapidT2", "SniperMediumT4", "SniperMediumT5", "SupportHeavyT4", "SupportHeavyT5", "SupportLightEasyT3", "SupportLightStasisT4", "SupportLightStasisT5", "SupportLightT3", "SupportLightTractorT4", "SupportLightTractorT5", "SupportMediumBeamAmpT2", "SupportMediumBeamAmpT3", "SupportMediumBoss01", "SupportMediumEasyT1", "SupportMediumHPDroneT4", "SupportMediumHPDroneT5", "TitanCarrierT4", "TitanCarrierT5", "TitanCarrierT6"}

func cmdEnemy(c *chatCommandCtx) string {
	if len(c.args) == 0 {
		return "Usage: /enemy <ShipId>|off|list -- e.g. /enemy TitanCarrierT6"
	}
	arg := c.args[0]
	if strings.EqualFold(arg, "list") {
		return "Enemy ships: " + strings.Join(havocEnemyIDs, ", ")
	}
	if strings.EqualFold(arg, "off") {
		arg = "off"
	} else {
		found := ""
		for _, id := range havocEnemyIDs {
			if strings.EqualFold(id, arg) {
				found = id
			}
		}
		if found == "" {
			return "Unknown ship " + arg + ". /enemy list shows the valid ones."
		}
		arg = found
	}
	target, where, ok := matchCommandTarget(c)
	if !ok {
		return noLiveBattleReply("/enemy " + arg)
	}
	queueHostCommand(target, "enemy "+arg)
	return "/enemy " + arg + " sent to " + where + "."
}

// cmdSpawn queues "spawn <ShipId> <n> <mult>": n enemies of that ship join
// the current wave now, mult>1 = elite (xN health, more damage; host
// EliteTick). "boss" picks one of the map's *Boss01 rows at random.
func cmdSpawn(c *chatCommandCtx) string {
	return spawnCommand(c, c.args, 1, "/spawn <ShipId>|boss [1-8] [x2-x10]")
}

// cmdBoss is /spawn for one elite: /boss [ShipId|random] [2-10], default a
// random *Boss01 row at x2; "x3" works as the multiplier too. A bare number
// first is a custom boss id from the admin dashboard's Bosses tab
// (admin_bosses.go, operator 2026-10-19: "/boss 1 is always that one boss",
// "/boss 1 3" overrides its multiplier); it queues "bossdef <id> <mult>" and
// the host fetches the definition. /boss list names them.
func cmdBoss(c *chatCommandCtx) string {
	if len(c.args) > 0 && strings.EqualFold(c.args[0], "list") {
		defs := bossList()
		if len(defs) == 0 {
			return "No custom bosses yet -- make some in the admin dashboard's Bosses tab."
		}
		parts := make([]string, 0, len(defs))
		for _, d := range defs {
			parts = append(parts, fmt.Sprintf("%d %s (x%d)", d.ID, d.Name, d.Mult))
		}
		return "Bosses: " + strings.Join(parts, ", ")
	}
	if len(c.args) > 0 {
		if id, err := strconv.Atoi(c.args[0]); err == nil {
			return customBossCommand(c, id, c.args[1:])
		}
	}
	// No ShipId: a random custom boss when there are any (a bare /boss gave a
	// stock SniperHeavyBoss01 instead of the dashboard's bosses, operator
	// 2026-10-21); "/boss stock" keeps the random stock *Boss01.
	stock := len(c.args) > 0 && strings.EqualFold(c.args[0], "stock")
	if stock {
		c.args = c.args[1:]
	}
	if defs := bossList(); !stock && len(defs) > 0 {
		var rest []string
		named := false
		for i, a := range c.args {
			l := strings.ToLower(a)
			if l == "random" || l == "boss" {
				continue
			}
			if i == 0 && !strings.HasPrefix(l, "x") {
				named = true // a ShipId
				break
			}
			rest = append(rest, l)
		}
		if !named {
			return customBossCommand(c, defs[rand.Intn(len(defs))].ID, rest)
		}
	}
	var args []string
	for i, a := range c.args {
		l := strings.ToLower(a)
		if i == 0 && l != "random" && l != "boss" && !strings.HasPrefix(l, "x") {
			args = append(args, a) // a ShipId
			continue
		}
		if l == "random" || l == "boss" {
			continue
		}
		if _, err := strconv.Atoi(l); err == nil {
			l = "x" + l
		}
		args = append(args, l)
	}
	if len(args) == 0 || strings.HasPrefix(args[0], "x") {
		args = append([]string{"boss"}, args...)
	}
	return spawnCommand(c, args, 2, "/boss [<id>|ShipId|random|stock] [2-10]")
}

func customBossCommand(c *chatCommandCtx, id int, rest []string) string {
	d, ok := bossByID(id)
	if !ok {
		return fmt.Sprintf("No boss %d. /boss list shows the custom bosses.", id)
	}
	mult := d.Mult
	if len(rest) > 1 {
		return "Usage: /boss <id> [1-10]"
	}
	if len(rest) == 1 {
		v, err := strconv.Atoi(strings.TrimPrefix(strings.ToLower(rest[0]), "x"))
		if err != nil || v < 1 || v > 10 {
			return "Usage: /boss <id> [1-10]"
		}
		mult = v
	}
	target, where, ok := matchCommandTarget(c)
	if !ok {
		return noLiveBattleReply(fmt.Sprintf("/boss %d", id))
	}
	queueHostCommand(target, fmt.Sprintf("bossdef %d %d", id, mult))
	return fmt.Sprintf("Boss %d %s (x%d) sent to %s.", id, d.Name, mult, where)
}

func spawnCommand(c *chatCommandCtx, args []string, mult int, usage string) string {
	if len(args) == 0 {
		return "Usage: " + usage + " -- e.g. /spawn TitanCarrierT6 2 x3"
	}
	ship := ""
	if strings.EqualFold(args[0], "boss") {
		var bosses []string
		for _, id := range havocEnemyIDs {
			if strings.HasSuffix(id, "Boss01") {
				bosses = append(bosses, id)
			}
		}
		ship = bosses[rand.Intn(len(bosses))]
	} else {
		for _, id := range havocEnemyIDs {
			if strings.EqualFold(id, args[0]) {
				ship = id
			}
		}
	}
	if ship == "" {
		return "Unknown ship " + args[0] + ". /enemy list shows the valid ones."
	}
	n := 1
	for _, a := range args[1:] {
		a = strings.ToLower(a)
		if strings.HasPrefix(a, "x") {
			v, err := strconv.Atoi(a[1:])
			if err != nil || v < 1 || v > 10 {
				return "Usage: " + usage
			}
			mult = v
			continue
		}
		v, err := strconv.Atoi(a)
		if err != nil || v < 1 || v > 8 {
			return "Usage: " + usage
		}
		n = v
	}
	target, where, ok := matchCommandTarget(c)
	if !ok {
		return noLiveBattleReply("/spawn " + ship)
	}
	queueHostCommand(target, fmt.Sprintf("spawn %s %d %d", ship, n, mult))
	elite := ""
	if mult > 1 {
		elite = fmt.Sprintf(" (elite x%d)", mult)
	}
	return fmt.Sprintf("/spawn %s x%d%s sent to %s.", ship, n, elite, where)
}

// Bunnies for /bunny: data/chat/bunnies.txt when it has any, one bunny per
// block, blocks separated by a line "---" (read on every call, so edits apply
// without a restart); else the built-ins below. The chat font is proportional
// and leading spaces may be trimmed, so art relies on shape, not columns.
var chatBunnies = [][]string{
	{`(\(\`, `( -.-)`, `o_(")(")`},
	{`(\_/)`, `(o.o)`, `(> <)`},
	{`(\__/)`, `(='.'=)`, `(")_(")`},
	{`(\(\`, `(^.^)`, `c(")(")`},
	{`(\ /)`, `( . .)`, `c(")(")`},
}

// chatBunnyPick is swapped by tests for a fixed choice.
var chatBunnyPick = func(n int) int { return rand.Intn(n) }

func chatBunnyArt() [][]string {
	lines, err := readChatTextFile("bunnies.txt")
	if err != nil {
		return chatBunnies
	}
	var arts [][]string
	var cur []string
	flush := func() {
		for len(cur) > 0 && strings.TrimSpace(cur[0]) == "" {
			cur = cur[1:]
		}
		for len(cur) > 0 && strings.TrimSpace(cur[len(cur)-1]) == "" {
			cur = cur[:len(cur)-1]
		}
		if len(cur) > 0 {
			arts = append(arts, cur)
		}
		cur = nil
	}
	for _, l := range lines {
		if strings.TrimSpace(l) == "---" {
			flush()
			continue
		}
		cur = append(cur, l)
	}
	flush()
	if len(arts) == 0 {
		return chatBunnies
	}
	return arts
}

// cmdBunny posts one bunny to everyone in the room it was typed in: the art
// and nothing else (empty reply = no Server line back to the typist).
func cmdBunny(c *chatCommandCtx) string {
	arts := chatBunnyArt()
	socialHubInstance.announceIn(c.channel, strings.Join(arts[chatBunnyPick(len(arts))], "\n"))
	return ""
}

// cmdTeam queues "team <pid> <n>" for the host mod, which applies it in its
// team sync (FixTeam), also on the ship in flight. The player is matched by
// display name among online players: exact first, then unique prefix.
// onlinePlayerByName finds one online player by exact name, else by unique
// name prefix; the string is the refusal to show when that fails.
func onlinePlayerByName(name string) (onlinePlayer, string) {
	want := strings.ToLower(name)
	var exact, prefix []onlinePlayer
	for _, p := range socialHubInstance.onlinePlayers() {
		n := strings.ToLower(p.Name)
		if n == want {
			exact = append(exact, p)
		} else if strings.HasPrefix(n, want) {
			prefix = append(prefix, p)
		}
	}
	if len(exact) == 0 {
		exact = prefix
	}
	if len(exact) == 0 {
		return onlinePlayer{}, "No online player matches " + name + "."
	}
	if len(exact) != 1 {
		return onlinePlayer{}, "More than one player matches " + name + "."
	}
	return exact[0], ""
}

func cmdTeam(c *chatCommandCtx) string {
	if len(c.args) != 2 || (c.args[1] != "1" && c.args[1] != "2") {
		return "Usage: /team <player> <1|2>"
	}
	// Resolve the target before the player lookup: an undeliverable command
	// should say so even when the name is also wrong, and it needs no state.
	target, where, ok := matchCommandTarget(c)
	if !ok {
		return noLiveBattleReply("/team " + c.args[0] + " " + c.args[1])
	}
	p, bad := onlinePlayerByName(c.args[0])
	if bad != "" {
		return bad
	}
	queueHostCommand(target, "team "+strings.ReplaceAll(p.PID, "-", "")+" "+c.args[1])
	return p.Name + " moves to team " + c.args[1] + " in " + where + "."
}

// cmdNuke: /nuke has the host mod apply lethal damage to every non-human ship
// except Onslaught command ships (CommandNuke). It first ran the game's
// ServerKillAllEnemiesInPVE debug RPC, which only clears Havoc waves.
func cmdNuke(c *chatCommandCtx) string {
	queueHostCommand("", "nuke")
	return "Every enemy ship is destroyed."
}

// cmdMeteor: /meteor [n] [distance]. In Onslaught the host mod sends one
// nuker herald that fires n salvos (default 3) with no cooldown, distance uu from the first ship
// (default 6000; uu are cm), on team [team] (default 0, neutral);
// elsewhere n Ultra torpedoes rain on the ships (default 12)
// launched distance uu up (CommandHeraldMeteor / CommandSkyMeteor).
func cmdMeteor(c *chatCommandCtx) string {
	const usage = "Usage: /meteor [1-40] [distance 1000-5000000] [team 0-255]"
	cmd := "meteor"
	if len(c.args) > 0 {
		v, err := strconv.Atoi(c.args[0])
		if err != nil || v < 1 || v > 40 {
			return usage
		}
		cmd += " " + strconv.Itoa(v)
	}
	if len(c.args) > 1 {
		h, err := strconv.Atoi(c.args[1])
		if err != nil || h < 1000 || h > 5000000 {
			return usage
		}
		cmd += " " + strconv.Itoa(h)
	}
	if len(c.args) > 2 {
		t, err := strconv.Atoi(c.args[2])
		if err != nil || t < 0 || t > 255 {
			return usage
		}
		cmd += " " + strconv.Itoa(t)
	}
	queueHostCommand("", cmd)
	return "Meteor incoming."
}

// cmdSwarm is /swarm and /squad: "<cmd> n team" to every running match.
func cmdSwarm(cmd string, max int) func(*chatCommandCtx) string {
	return func(c *chatCommandCtx) string {
		// Default count, cap and team from the admin Waves tab (admin_waves.go).
		cfg := waveCfg()
		def, team, lim := cfg.SwarmDefault, cfg.Team, min(max, cfg.SwarmMax)
		if cmd == "squad" {
			def, lim = cfg.SquadDefault, min(max, cfg.SquadMax)
		}
		usage := fmt.Sprintf("Usage: /%s [1-%d] [team 0-4]", cmd, lim)
		if len(c.args) > 2 {
			return usage
		}
		n := def
		for i, a := range c.args {
			v, err := strconv.Atoi(a)
			if err != nil || v < 0 || (i == 0 && (v < 1 || v > lim)) || (i == 1 && v > 4) {
				return usage
			}
			if i == 0 {
				n = v
			} else {
				team = v
			}
		}
		line := fmt.Sprintf("%s %d %d", cmd, n, team)
		target, where, ok := matchCommandTarget(c)
		if !ok {
			return noLiveBattleReply("/" + cmd)
		}
		queueHostCommand(target, line)
		return fmt.Sprintf("/%s sent to %s.", cmd, where)
	}
}

// cmdWave is a faux Havoc wave: /boss (random custom boss, else a stock one)
// plus /squad and /swarm at their defaults.
var havocWaveCmd = hostCmdCheck("wave", "[x2-x10] | skip|1-50", func(a string) bool {
	n, err := strconv.Atoi(a)
	return a == "skip" || (err == nil && n >= 1 && n <= 50)
})

func cmdWave(c *chatCommandCtx) string {
	if len(c.args) == 1 && !strings.HasPrefix(strings.ToLower(c.args[0]), "x") {
		return havocWaveCmd(c)
	}
	if len(c.args) > 1 {
		return "Usage: /wave [x2-x10] | skip|1-50"
	}
	target, where, ok := matchCommandTarget(c)
	if !ok {
		return noLiveBattleReply("/wave")
	}
	cc := *c
	boss := cmdBoss(&cc)
	if !strings.Contains(boss, "sent to") {
		return boss
	}
	// Composition from the admin Waves tab (admin_waves.go); 0 leaves a part out.
	cfg := waveCfg()
	var parts []string
	if cfg.WaveSquad > 0 {
		queueHostCommand(target, fmt.Sprintf("squad %d %d", cfg.WaveSquad, cfg.Team))
		parts = append(parts, fmt.Sprintf("%d assault ships", cfg.WaveSquad))
	}
	if cfg.WaveSwarm > 0 {
		queueHostCommand(target, fmt.Sprintf("swarm %d %d", cfg.WaveSwarm, cfg.Team))
		parts = append(parts, fmt.Sprintf("%d fighters", cfg.WaveSwarm))
	}
	if cfg.WaveHunters > 0 {
		queueHostCommand(target, fmt.Sprintf("hunters %d 0 all", cfg.WaveHunters))
		parts = append(parts, fmt.Sprintf("%d hunters", cfg.WaveHunters))
	}
	what := strings.TrimSuffix(strings.SplitN(boss, " sent to", 2)[0], ".")
	if len(parts) > 0 {
		what += " + " + strings.Join(parts, " + ")
	}
	return fmt.Sprintf("Wave incoming: %s, %s.", what, where)
}

func cmdFighters(c *chatCommandCtx) string {
	cmd := "fighters"
	if len(c.args) > 0 {
		t, err := strconv.Atoi(c.args[0])
		if err != nil || t < 0 || t > 255 || len(c.args) > 1 {
			return "Usage: /fighters [team 0-255]"
		}
		cmd += " " + strconv.Itoa(t)
	}
	queueHostCommand("", cmd)
	return "Fighters incoming."
}

func cmdHunters(c *chatCommandCtx) string {
	cmd := "hunters"
	if len(c.args) > 2 {
		return "Usage: /hunters [1-12] [team 0-255]"
	}
	for i, a := range c.args {
		v, err := strconv.Atoi(a)
		if err != nil || (i == 0 && (v < 1 || v > 12)) || (i == 1 && (v < 0 || v > 255)) {
			return "Usage: /hunters [1-12] [team 0-255]"
		}
		cmd += " " + strconv.Itoa(v)
	}
	if len(c.args) == 0 {
		cmd += " " + strconv.Itoa(waveCfg().HuntersDefault)
	}
	queueHostCommand("", cmd)
	return "Hunters incoming."
}

func cmdProps(c *chatCommandCtx) string {
	if len(c.args) < 1 || len(c.args) > 2 {
		return "Usage: /props <object> [filter]"
	}
	for _, a := range c.args {
		if strings.ContainsAny(a, " \t\r\n") {
			return "Usage: /props <object> [filter]"
		}
	}
	queueHostCommand("", "props "+strings.Join(c.args, " "))
	return "Props logged to the mod log."
}

// cmdHost: /host <line> queues a raw host-mod command (probefn and other
// debug verbs that have no chat command of their own).
func cmdHost(c *chatCommandCtx) string {
	if len(c.args) == 0 {
		return "Usage: /host <raw command>"
	}
	queueHostCommand("", strings.Join(c.args, " "))
	return "Sent to the battle server."
}

// cmdKill: /kill me|all|<player> destroys ships through the game's own
// ServerKillMe debug RPC; the player respawns as after any death.
func cmdKill(c *chatCommandCtx) string {
	if len(c.args) != 1 {
		return "Usage: /kill me|all|<player>"
	}
	// /team and /kill used to queue with an empty target whatever room they
	// were typed in, i.e. they always broadcast; see matchCommandTarget.
	target, where, ok := matchCommandTarget(c)
	if !ok {
		return noLiveBattleReply("/kill " + c.args[0])
	}
	switch strings.ToLower(c.args[0]) {
	case "all":
		queueHostCommand(target, "kill all")
		return "Every player's ship in " + where + " is destroyed."
	case "me":
		queueHostCommand(target, "kill "+strings.ReplaceAll(c.pid, "-", ""))
		return "Your ship is destroyed."
	}
	p, bad := onlinePlayerByName(c.args[0])
	if bad != "" {
		return bad
	}
	queueHostCommand(target, "kill "+strings.ReplaceAll(p.PID, "-", ""))
	return p.Name + "'s ship is destroyed."
}
