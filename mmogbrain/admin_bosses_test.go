package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func withBosses(t *testing.T, defs ...bossDef) {
	t.Helper()
	bossStore.Lock()
	old, oldLoaded := bossStore.defs, bossStore.loaded
	bossStore.defs, bossStore.loaded = map[int]bossDef{}, true
	for _, d := range defs {
		bossStore.defs[d.ID] = d
	}
	bossStore.Unlock()
	t.Cleanup(func() {
		bossStore.Lock()
		bossStore.defs, bossStore.loaded = old, oldLoaded
		bossStore.Unlock()
	})
}

func gorgon() bossDef {
	return bossDef{ID: 1, Name: "Gorgon", Tier: "legendary", Class: "Dreadnought", Manufacturer: "JupiterArms",
		Base: "DreadnoughtHeavyNukerT5", Mult: 3, Paint: 385810647,
		Modules: []string{"/Game/Generic/Abilities/Dreadnought/Pri_WarpM_Dmg/AB_DN_Pri_WarpM_Dmg_Ability_BP.AB_DN_Pri_WarpM_Dmg_Ability_BP_C"},
		Feats:   []bossFeat{{"HVC_Boost_Def_ArInc_Large", 3}}}
}

func TestBossValidation(t *testing.T) {
	d := gorgon()
	if err := d.validate(); err != nil {
		t.Fatal(err)
	}
	for name, mut := range map[string]func(*bossDef){
		"maker builds another size": func(d *bossDef) { d.Manufacturer = "Oberon" },
		"tier outside group":        func(d *bossDef) { d.Tier = "recruit" },
		"paint is a decal":          func(d *bossDef) { d.Paint = 24<<24 | 5 },
		"feat not a boost":          func(d *bossDef) { d.Feats = []bossFeat{{"DN_Something", 1}} },
		"mult":                      func(d *bossDef) { d.Mult = 11 },
		"name with newline":         func(d *bossDef) { d.Name = "a\nship=x" },
		"module not a class":        func(d *bossDef) { d.Modules = []string{"rm -rf"} },
	} {
		d := gorgon()
		mut(&d)
		if d.validate() == nil {
			t.Errorf("%s: accepted", name)
		}
	}
}

func TestBossChatAndHost(t *testing.T) {
	t.Setenv("DN_ADMINS", "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4")
	withBosses(t, gorgon())
	admin := "a0a0a0a0b1b1c2c2d3d3e4e4e4e4e4e4"
	for _, tc := range []struct{ in, want string }{
		{"/boss 1", " bossdef 1 3"},
		{"/boss 1 5", " bossdef 1 5"},
		{"/boss 1 x2", " bossdef 1 2"},
	} {
		base := queueHostCommand("other-match", "noop x")
		out, _ := handleChatCommand(admin, "dreadnought.global", tc.in)
		got := hostCommandsSince("m1", base)
		if len(got) != 1 || !strings.HasSuffix(got[0], tc.want) {
			t.Fatalf("%s: reply %q, queued %q, want suffix %q", tc.in, out, got, tc.want)
		}
	}
	for _, bad := range []string{"/boss 2", "/boss 1 11", "/boss 1 2 3"} {
		base := queueHostCommand("other-match", "noop x")
		out, _ := handleChatCommand(admin, "dreadnought.global", bad)
		if got := hostCommandsSince("m1", base); len(got) != 0 {
			t.Fatalf("%s queued %q (reply %q)", bad, got, out)
		}
	}
	if out, _ := handleChatCommand(admin, "dreadnought.global", "/boss list"); !strings.Contains(out, "1 Gorgon (x3)") {
		t.Errorf("/boss list: %q", out)
	}

	req := httptest.NewRequest(http.MethodGet, "/battle/boss?id=1", nil)
	req.RemoteAddr = "127.0.0.1:5555"
	rec := httptest.NewRecorder()
	battleBossHandler(rec, req)
	body := rec.Body.String()
	for _, want := range []string{"ship=DreadnoughtHeavyNukerT5\n", "name=Gorgon\n", "paint=385810647\n",
		"abilities=/Game/Generic/Abilities/Dreadnought/Pri_WarpM_Dmg/", "feats=HVC_Boost_Def_ArInc_Large*3\n"} {
		if !strings.Contains(body, want) {
			t.Errorf("host text lacks %q:\n%s", want, body)
		}
	}
}

func TestBossCatalog(t *testing.T) {
	c := buildBossCatalog()
	if len(c.Bases) == 0 || len(c.Modules) < 100 || len(c.Paints) == 0 || len(c.Patterns) == 0 || len(c.Feats) < 30 {
		t.Fatalf("catalog thin: %d bases, %d modules, %d paints, %d patterns, %d feats",
			len(c.Bases), len(c.Modules), len(c.Paints), len(c.Patterns), len(c.Feats))
	}
	for _, cl := range bossClasses {
		for _, m := range bossManufacturers {
			if c.HullLines[cl.ID][m] == "" {
				t.Errorf("%s/%s has no hull line", cl.ID, m)
			}
		}
	}
	for _, b := range c.Bases {
		if b.ID == "DreadnoughtHeavyNukerT5" && len(b.Modules) == 0 {
			t.Error("DreadnoughtHeavyNukerT5 has no dataset modules")
		}
	}
	t.Logf("%d bases, %d modules, %d paints, %d patterns, %d decals, %d emblems, %d feats; paint[0]=%+v",
		len(c.Bases), len(c.Modules), len(c.Paints), len(c.Patterns), len(c.Decals), len(c.Emblems), len(c.Feats), c.Paints[0])
}

func TestBossAIs(t *testing.T) {
	c := buildBossCatalog()
	byID := map[string]bossCatalogAI{}
	for _, a := range c.AIs {
		byID[a.ID] = a
	}
	if a := byID["TitanCarrier_behavior_PanC_Hard"]; a.Label != "Superheavy Carrier" || len(a.Ships) != 3 {
		t.Errorf("titan carrier AI: %+v", a)
	}
	if a := byID["DreadnoughtHeavy_Behavior_ArmoredNuker_Hard"]; a.Slots != 6 {
		t.Errorf("nuker slots %d", a.Slots)
	}
	d := bossDef{ID: 1, Name: "X", Tier: "legendary", Class: "Dreadnought", Manufacturer: "JupiterArms", Base: "DreadnoughtHeavyNukerT5", Mult: 3,
		Modules: make([]string, 7)}
	d.Modules[6] = "/Game/A/B.B_C"
	if err := d.validate(); err == nil {
		t.Error("7th module accepted on a 6-slot AI")
	}
	d.AI = "SniperMedium_Behavior_Standard_Hard"
	if err := d.validate(); err != nil {
		t.Errorf("13-slot AI: %v", err)
	}
	if !strings.Contains(bossHostText(d), "ai=/Game/") {
		t.Error("no ai= path in host text")
	}
	for _, a := range c.AIs {
		t.Logf("%-50s %2d %s", a.ID, a.Slots, a.Label)
	}
}
