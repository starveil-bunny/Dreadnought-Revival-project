//go:build windows

package main

import (
	"strings"
	"testing"
)

func TestSkins(t *testing.T) {
	for in, want := range map[string]string{"": "dreadnought", "classic": "classic", "bunny": "dreadnought"} {
		if got := skinName(launcherSettings{Skin: in}); got != want {
			t.Errorf("skinName(%q) = %q, want %q", in, got, want)
		}
	}
	if pageFor("classic") != desktopPageHTML || !strings.Contains(pageFor("dreadnought"), "dnLiveResult") {
		t.Fatal("pageFor picks the wrong page")
	}
	for _, id := range []string{"dnfont", "ptitle", "ico-cr", "ico-gold"} {
		if !strings.Contains(dreadnoughtPageHTML, `id="`+id+`"`) {
			t.Errorf("the page has no #%s", id)
		}
	}
	// NavigateToString's limit; media is fetched separately for this reason.
	if len(dreadnoughtPageHTML) > 1<<20 {
		t.Fatalf("page is %d bytes", len(dreadnoughtPageHTML))
	}
	a := &launcherAPI{}
	if findGameBinary("", Config{}) == "" {
		if a.Media("logo.png") != "" || len(a.Backgrounds()) != 0 {
			t.Fatal("art without game files")
		}
		return
	}
	if bgs := a.Backgrounds(); len(bgs) == 0 || !strings.HasPrefix(a.Media(bgs[0]), "data:image/jpeg;base64,") {
		t.Fatalf("backgrounds %v", bgs)
	}
	if !strings.HasPrefix(a.Media("logo.png"), "data:image/png;base64,") || a.Media("../skins_windows.go") != "" {
		t.Fatal("media lookup")
	}
	// The game's own font and the client's currency glyphs come from the paks and
	// the page fetches them by these exact names (loadDreadnoughtUI). A font
	// served with the wrong type, or a media file that went missing, leaves the
	// page silently on its fallback font -- so it is worth a test.
	if m := a.Media("Quantico-Regular.ttf"); !strings.HasPrefix(m, "data:font/ttf;base64,") {
		t.Errorf("font media: %.60q", m)
	} else if len(m) < 20000 {
		t.Errorf("font is only %d base64 bytes, the TTF is ~22KB", len(m))
	}
	for _, f := range []string{"credit.png", "gold.png"} {
		if m := a.Media(f); !strings.HasPrefix(m, "data:image/png;base64,") {
			t.Errorf("%s: %.60q", f, m)
		}
	}
}
