package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestChatCommandsAdminGate(t *testing.T) {
	admin := "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4"
	other := "c332d9b0be8d4844883be2496ef58b22"
	t.Setenv("DN_ADMINS", "a0a0a0a0-b1b1-c2c2-d3d3-e4e4e4e4e4e4")
	t.Setenv("DN_PVE_SEASON", "")
	noPvESeasonPin(t)

	if _, ok := handleChatCommand(other, "dreadnought.global", "hello /season"); ok {
		t.Fatal("a line not starting with / or ! is chat, not a command")
	}
	if out, ok := handleChatCommand(other, "dreadnought.global", "/season"); !ok || !strings.Contains(out, "season 1, episode") {
		t.Fatalf("anyone may read the season: %q", out)
	}
	if out, _ := handleChatCommand(other, "dreadnought.global", "/season 3"); !strings.Contains(out, "admins only") {
		t.Fatalf("setting the season needs DN_ADMINS: %q", out)
	}
	if out, _ := handleChatCommand(admin, "dreadnought.global", "!season 3 2"); !strings.Contains(out, "season 3, episode 2") {
		t.Fatalf("admin /season 3 2: %q", out)
	}
	if out, _ := handleChatCommand(admin, "dreadnought.global", "!season next"); !strings.Contains(out, "season 3, episode 3") {
		t.Fatalf("admin /season next: %q", out)
	}
	if out, _ := handleChatCommand(other, "dreadnought.global", "/help"); strings.Contains(out, "/turbo") {
		t.Fatalf("non-admin help lists admin commands: %q", out)
	}
	if out, _ := handleChatCommand(admin, "dreadnought.global", "/help"); !strings.Contains(out, "/turbo") {
		t.Fatalf("admin help misses admin commands: %q", out)
	}
}

func TestChatCommandXPMultiplier(t *testing.T) {
	t.Setenv("DN_ADMINS", "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4")
	t.Setenv("DN_REWARD_XP_MULT", "")
	handleChatCommand("a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4", "dreadnought.global", "/xp 2")
	if matchXPMultiplier() != 2 {
		t.Fatalf("multiplier %v", matchXPMultiplier())
	}
}

func TestHostCommandQueue(t *testing.T) {
	t.Setenv("DN_ADMINS", "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4")
	base := queueHostCommand("other-match", "noop x")
	out, _ := handleChatCommand("a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4", "dreadnought.global", "/turbo on")
	if !strings.Contains(out, "every running match") {
		t.Fatalf("hangar /turbo: %q", out)
	}
	got := hostCommandsSince("m1", base)
	if len(got) != 1 || !strings.HasSuffix(got[0], " turbo on") {
		t.Fatalf("host m1 since %d: %q", base, got)
	}
	if got := hostCommandsSince("m1", base-1); len(got) != 1 {
		t.Fatalf("another match's command leaked: %q", got)
	}
}

func TestMatchIDFromChannel(t *testing.T) {
	for in, want := range map[string]string{
		"abc-123.all": "abc-123", "abc-123-2.team": "abc-123", // joinMatchChannels naming, not the dropped "-t2"
		"abc-123.team": "abc", "dreadnought.global": "", "42.squad": "",
	} {
		if got := matchIDFromChannel(in); got != want {
			t.Errorf("%q -> %q, want %q", in, got, want)
		}
	}
	if _, ok := chatChannelType(matchTeamChannel("fdf82dd4-9eeb-4e7d-b370-85cb68a9ba0e", 1)); !ok {
		t.Error("team room name must classify")
	}
}

func TestSpawnAndBossCommands(t *testing.T) {
	t.Setenv("DN_ADMINS", "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4")
	admin := "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4"
	for _, tc := range []struct{ in, want string }{
		{"/spawn titancarriert6 2 x3", " spawn TitanCarrierT6 2 3"},
		{"/spawn TitanCarrierT6", " spawn TitanCarrierT6 1 1"},
		{"/boss SniperHeavyBoss01", " spawn SniperHeavyBoss01 1 2"},
		{"/boss stock x7", "Boss01 1 7"},
		{"/boss stock boss 3", "Boss01 1 3"},
		{"/boss stock random 5", "Boss01 1 5"},
		{"/boss SniperHeavyBoss01 4", " spawn SniperHeavyBoss01 1 4"},
	} {
		base := queueHostCommand("other-match", "noop x")
		out, _ := handleChatCommand(admin, "dreadnought.global", tc.in)
		got := hostCommandsSince("m1", base)
		if len(got) != 1 || !strings.HasSuffix(got[0], tc.want) {
			t.Fatalf("%s: reply %q, queued %q, want suffix %q", tc.in, out, got, tc.want)
		}
	}
	for _, bad := range []string{"/spawn NotAShip", "/spawn TitanCarrierT6 9", "/boss x11"} {
		base := queueHostCommand("other-match", "noop x")
		out, _ := handleChatCommand(admin, "dreadnought.global", bad)
		if got := hostCommandsSince("m1", base); len(got) != 0 {
			t.Fatalf("%s queued %q (reply %q)", bad, got, out)
		}
	}
}

func TestBunnyCommand(t *testing.T) {
	t.Skip("/bunny is off (chat_commands.go): the client font lacks glyphs for the art")
	old := chatBunnyPick
	chatBunnyPick = func(int) int { return 1 }
	defer func() { chatBunnyPick = old }()
	// Not admin-only: any player may post a bunny.
	// The bunny itself is the only output: no reply line to the typist.
	out, ok := handleChatCommand("someone", "dreadnought.global", "!bunny hello there")
	if !ok || out != "" {
		t.Fatalf("reply %q handled %v", out, ok)
	}
	for _, art := range chatBunnies {
		for _, l := range art {
			if l == "" || l[0] == ' ' {
				t.Fatalf("bunny line %q starts blank (chat may trim it)", l)
			}
		}
	}
}

func TestShakeAndTeamCommands(t *testing.T) {
	t.Setenv("DN_ADMINS", "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4")
	admin := "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4"
	base := queueHostCommand("other-match", "noop x")
	handleChatCommand(admin, "dreadnought.global", "/shake 7")
	if got := hostCommandsSince("m1", base); len(got) != 1 || !strings.HasSuffix(got[0], " shake 7") {
		t.Fatalf("/shake 7 queued %q", got)
	}
	base = queueHostCommand("other-match", "noop x")
	handleChatCommand(admin, "dreadnought.global", "/flash pink")
	if got := hostCommandsSince("m1", base); len(got) != 1 || !strings.HasSuffix(got[0], " flash pink") {
		t.Fatalf("/flash pink queued %q", got)
	}
	base = queueHostCommand("other-match", "noop x")
	handleChatCommand(admin, "dreadnought.global", "/slowmo 0.5")
	if got := hostCommandsSince("m1", base); len(got) != 1 || !strings.HasSuffix(got[0], " slowmo 0.5") {
		t.Fatalf("/slowmo 0.5 queued %q", got)
	}
	for in, want := range map[string]string{"/kill me": " kill " + admin, "/kill all": " kill all", "/nuke": " nuke"} {
		base = queueHostCommand("other-match", "noop x")
		handleChatCommand(admin, "dreadnought.global", in)
		if got := hostCommandsSince("m1", base); len(got) != 1 || !strings.HasSuffix(got[0], want) {
			t.Fatalf("%s queued %q", in, got)
		}
	}
	for _, bad := range []string{"/kill", "/kill nobody-online", "/slowmo 0.1", "/slowmo 2", "/flash purple", "/flash", "/shake 11", "/shake", "/team", "/team someone 3", "/team nobody-online 1"} {
		base := queueHostCommand("other-match", "noop x")
		out, _ := handleChatCommand(admin, "dreadnought.global", bad)
		if got := hostCommandsSince("m1", base); len(got) != 0 {
			t.Fatalf("%s queued %q (reply %q)", bad, got, out)
		}
	}
}

func TestHelpFilterAndFile(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("DATA_DIR", dir)
	os.MkdirAll(filepath.Join(dir, "chat"), 0o755)
	os.WriteFile(filepath.Join(dir, "chat", "help.txt"), []byte("// note\nHi\n/bunny -- b\n[admin]\n/nuke -- n\n"), 0o644)
	t.Setenv("DN_ADMINS", "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4")
	if out, _ := handleChatCommand("x", "dreadnought.global", "/help"); out != "Hi\n/bunny -- b" {
		t.Fatalf("player help %q", out)
	}
	if out, _ := handleChatCommand("a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4", "dreadnought.global", "/help nuke"); out != "/nuke -- n" {
		t.Fatalf("filtered admin help %q", out)
	}
	os.WriteFile(filepath.Join(dir, "chat", "bunnies.txt"), []byte("// n\nA\nB\n---\n\nC\n"), 0o644)
	if arts := chatBunnyArt(); len(arts) != 2 || arts[0][1] != "B" || arts[1][0] != "C" {
		t.Fatalf("bunnies %q", arts)
	}
}

// /turbo and /npc are hostCmd wrappers (chat_commands.go:81-82), so the only
// Go-side logic is the allowed-word check and where the line is queued: the
// host does the work (battle-server-mod CommandsTick, turbo on|off,
// npc normal|hard|brutal -> tier 0/1/2). These pin the parsing and the error
// paths; nothing here needs a live match.
//
// The commands themselves HAVE run live, from Global, against TDM and Conquest
// (/tmp/testq4.log, 2026-10-17 08:39: "/npc brutal sent to every running
// match." then the host's "commands: #15 npc brutal"; /tmp/testq12.log the
// same for /turbo) -- the parsing below is what those runs exercised. What was
// never exercised is the in-room targeting, which is the other test in this
// file: every live call so far was typed in Global.
func TestTurboAndNpcCommandParsing(t *testing.T) {
	t.Setenv("DN_ADMINS", "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4")
	t.Setenv("DN_CHAT_COMMANDS", "")
	const admin = "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4"

	queued := func(line string) (string, []string) {
		base := queueHostCommand("other-match", "noop x")
		out, ok := handleChatCommand(admin, "dreadnought.global", line)
		if !ok {
			t.Fatalf("%s: not handled as a command", line)
		}
		return out, hostCommandsSince("m1", base)
	}

	for _, tc := range []struct{ in, want string }{
		{"/turbo on", " turbo on"},
		{"/turbo off", " turbo off"},
		{"/Turbo ON", " turbo on"}, // command name and argument are both folded
		{"/npc normal", " npc normal"},
		{"/npc hard", " npc hard"},
		{"/NPC Brutal", " npc brutal"},
		{"/turbo on now", " turbo on"}, // extra words are dropped, not rejected
	} {
		out, got := queued(tc.in)
		if len(got) != 1 || !strings.HasSuffix(got[0], tc.want) {
			t.Errorf("%s: queued %q (reply %q), want one line ending %q", tc.in, got, out, tc.want)
			continue
		}
		if !strings.Contains(out, "every running match") {
			t.Errorf("%s: reply %q, want the Global wording", tc.in, out)
		}
	}

	// Usage, never a queued line. /npc off is not a reset: "normal" is
	// (host tier 0), which is why it is absent from the allowed set.
	for _, bad := range []string{"/turbo", "/turbo enable", "/turbo 1", "/npc", "/npc off", "/npc brutal2"} {
		out, got := queued(bad)
		if len(got) != 0 {
			t.Errorf("%s: queued %q, want nothing (reply %q)", bad, got, out)
		}
		if !strings.HasPrefix(out, "Usage: /") {
			t.Errorf("%s: reply %q, want a usage line", bad, out)
		}
	}

	for _, line := range []string{"/turbo on", "/npc hard"} {
		base := queueHostCommand("other-match", "noop x")
		out, ok := handleChatCommand("c332d9b0be8d4844883be2496ef58b22", "dreadnought.global", line)
		if !ok || !strings.Contains(out, "admins only") {
			t.Errorf("%s as non-admin: %q (handled %v), want admins only", line, out, ok)
		}
		if got := hostCommandsSince("m1", base); len(got) != 0 {
			t.Errorf("%s as non-admin queued %q", line, got)
		}
	}
}

// A match room exists from matchmaking, before its battle does, and
// battleMatchIDFor also comes back empty when the state DB is unavailable. An
// empty target is not "this match": queueHostCommand("") hands the line to
// every running match (hostCommandsSince matches any id), so the reply has to
// stop promising one match.
func TestHostCommandInMatchRoomWithoutBattle(t *testing.T) {
	t.Setenv("DN_ADMINS", "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4")
	const admin = "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4"
	const room = "fdf82dd4-9eeb-4e7d-b370-85cb68a9ba0e.all"

	base := queueHostCommand("other-match", "noop x")
	out, _ := handleChatCommand(admin, room, "/npc hard")
	if got := hostCommandsSince("m1", base); len(got) != 0 {
		t.Errorf("queued %q to every match, want nothing: the room has no battle (reply %q)", got, out)
	}
	if !strings.Contains(out, "no live battle") {
		t.Errorf("reply %q, want it to say this match has no battle yet", out)
	}
}

// The hand-rolled match-scoped commands had the same shape b9e17ac fixed in
// hostCmdCheck: an empty resolve meant "every running match" while the reply
// named one. /team and /kill went further and queued with an empty target
// whatever room they were typed in, so they always broadcast.
func TestHandRolledCommandsRefuseWithoutBattle(t *testing.T) {
	t.Setenv("DN_ADMINS", "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4")
	const admin = "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4"
	const room = "fdf82dd4-9eeb-4e7d-b370-85cb68a9ba0e.all"

	// A room whose battle does not exist yet (matchmaking creates the room
	// first) must refuse, not reach every match.
	for _, line := range []string{
		"/enemy TitanCarrierT6",
		"/spawn TitanCarrierT6",
		"/spawn TitanCarrierT6 2 x3",
		"/boss",
		"/boss SniperHeavyBoss01",
		"/team Someone 1",
		"/kill me",
		"/kill all",
		"/kill Someone",
	} {
		base := queueHostCommand("other-match", "noop x")
		out, ok := handleChatCommand(admin, room, line)
		if !ok {
			t.Fatalf("%s: not handled as a command", line)
		}
		if got := hostCommandsSince("m1", base); len(got) != 0 {
			t.Errorf("%s in a battle-less room queued %q to every match (reply %q)", line, got, out)
		}
		if !strings.Contains(out, "no live battle") {
			t.Errorf("%s: reply %q, want the no-live-battle refusal", line, out)
		}
	}

	// From Global they still go to every running match, as before.
	for _, tc := range []struct{ in, want string }{
		{"/enemy TitanCarrierT6", " enemy TitanCarrierT6"},
		{"/spawn TitanCarrierT6", " spawn TitanCarrierT6 1 1"},
		{"/spawn TitanCarrierT6 2 x3", " spawn TitanCarrierT6 2 3"},
		{"/boss SniperHeavyBoss01", " spawn SniperHeavyBoss01 1 2"},
		{"/kill me", " kill " + admin},
		{"/kill all", " kill all"},
	} {
		base := queueHostCommand("other-match", "noop x")
		out, _ := handleChatCommand(admin, "dreadnought.global", tc.in)
		got := hostCommandsSince("m1", base)
		if len(got) != 1 || !strings.HasSuffix(got[0], tc.want) {
			t.Errorf("%s from Global: queued %q (reply %q), want one line ending %q", tc.in, got, out, tc.want)
		}
	}
}
