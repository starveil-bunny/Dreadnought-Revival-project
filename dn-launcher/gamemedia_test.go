package main

import (
	"bytes"
	"image"
	_ "image/jpeg"
	_ "image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Against a real install when there is one (DN_PAKS, or Steam's default on
// Linux): every manifest entry extracts, at its size, and decodes.
func TestGameMediaFromPaks(t *testing.T) {
	paks := os.Getenv("DN_PAKS")
	if paks == "" {
		home, _ := os.UserHomeDir()
		paks = filepath.Join(home, ".local/share/Steam/steamapps/common/Dreadnought/DreadGame/DreadGame/Content/Paks")
	}
	if _, err := os.Stat(paks); err != nil {
		t.Skip("no game files")
	}
	g := newGameMedia(t.TempDir(), func() string { return paks })
	if n, err := g.ExtractAll(); err != nil || n != len(mediaManifest) {
		t.Fatalf("extracted %d/%d: %v", n, len(mediaManifest), err)
	}
	for name, item := range mediaManifest {
		b, err := g.Get(name)
		if err != nil {
			t.Fatal(name, err)
		}
		if item.raw {
			if !bytes.HasPrefix(b, []byte{0, 1, 0, 0}) {
				t.Errorf("%s is not a TTF", name)
			}
			continue
		}
		img, format, err := image.Decode(bytes.NewReader(b))
		if err != nil {
			t.Fatal(name, err)
		}
		if strings.HasSuffix(name, ".jpg") != (format == "jpeg") {
			t.Errorf("%s encoded as %s", name, format)
		}
		if s := img.Bounds().Size(); s.X != item.w || item.h != 0 && s.Y != item.h {
			t.Errorf("%s is %v, want %dx%d", name, s, item.w, item.h)
		}
	}
	if !g.Available("logo.png") || g.Available("../gamemedia.go") {
		t.Error("Available")
	}
}

func TestGameMediaNoGame(t *testing.T) {
	g := newGameMedia(t.TempDir(), func() string { return "" })
	if _, err := g.Get("logo.png"); err == nil || g.Available("logo.png") {
		t.Fatal("art without game files")
	}
}
