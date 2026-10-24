//go:build windows

package main

// Launcher skins: one set of actions (launcher_api_windows.go), several pages.
// "dreadnought" (default) is the compact game-launcher page in
// skin_dreadnought.html, dressed in the game's own art (the loading screens,
// logo, icons and font, read from the player's own paks: gamemedia.go);
// "classic" is the original page (desktopPageHTML).
//
// Media goes to the page through the API as data: URIs rather than inline in
// the HTML: the desktop window loads its page with NavigateToString, which
// refuses strings over 2 MB.

import (
	_ "embed"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

//go:embed skin_dreadnought.html
var dreadnoughtPageHTML string

var skins = []string{"dreadnought", "classic"}

func skinName(s launcherSettings) string {
	for _, k := range skins {
		if s.Skin == k {
			return k
		}
	}
	return skins[0]
}

func pageFor(skin string) string {
	if skin == "classic" {
		return desktopPageHTML
	}
	return dreadnoughtPageHTML
}

func (a *launcherAPI) SetSkin(name string) bool {
	settings := loadSettings()
	settings.Skin = name
	if skinName(settings) != name {
		return false
	}
	return saveSettings(settings) == nil
}

// launcherMedia is the page's art, extracted from the game the launcher
// would start (gamemedia.go).
var (
	launcherMediaOnce sync.Once
	launcherMedia     *gameMedia
)

func (a *launcherAPI) media() *gameMedia {
	launcherMediaOnce.Do(func() {
		launcherMedia = newGameMedia(filepath.Join(os.Getenv("LOCALAPPDATA"), "DreadnoughtPrivateServer"), func() string {
			return gamePaksDir(findGameBinary(a.exeDir, a.cfg))
		})
	})
	return launcherMedia
}

// gamePaksDir is Content\Paks beside the executable's Binaries\Win64.
func gamePaksDir(exe string) string {
	if exe == "" {
		return ""
	}
	return filepath.Join(filepath.Dir(exe), "..", "..", "Content", "Paks")
}

// Media returns the named art as a data: URI ("" if there is none).
func (a *launcherAPI) Media(name string) string {
	name = path.Base(name)
	b, err := a.media().Get(name)
	if err != nil {
		return ""
	}
	mime := "image/png"
	switch {
	case strings.HasSuffix(name, ".jpg"):
		mime = "image/jpeg"
	case strings.HasSuffix(name, ".ttf"):
		// The game's own font, for an @font-face (see the page's bootstrap).
		mime = "font/ttf"
	}
	return "data:" + mime + ";base64," + base64.StdEncoding.EncodeToString(b)
}

// Backgrounds lists the bg_*.jpg art the game files can supply.
func (a *launcherAPI) Backgrounds() []string {
	var out []string
	for _, n := range mediaNames() {
		if strings.HasPrefix(n, "bg_") && a.media().Available(n) {
			out = append(out, n)
		}
	}
	return out
}

const (
	liveStatusURL = "https://mmog.greybox.sixfoot.live/launcher/status"
	liveMeURL     = "https://mmog.greybox.sixfoot.live/launcher/me"
	skipTutURL    = "https://mmog.greybox.sixfoot.live/launcher/skip-tutorial"
	viewerBaseURL = "https://mmog.greybox.sixfoot.live/launcher"
)

// ViewerPage fetches the battle view page (mmogbrain launcherViewerPage) for
// the window to show in a frame.
func (a *launcherAPI) ViewerPage() map[string]any {
	status, body := a.ViewerAPI(http.MethodGet, "/viewer/page", "")
	if status != http.StatusOK {
		return map[string]any{"ok": false, "error": viewerErr(status, body)}
	}
	return map[string]any{"ok": true, "html": body}
}

// ViewerAPI carries one of the battle view's requests to the server: the
// page cannot reach it itself (the server's certificate is from our own CA,
// which this client trusts). Only /viewer and /fleet (the Fleet page,
// mmogbrain launcher_fleet.go) paths, as the signed-in player.
func launcherPagePath(p, root string) bool { return p == root || strings.HasPrefix(p, root+"/") }

func (a *launcherAPI) ViewerAPI(method, path, body string) (int, string) {
	a.mu.Lock()
	token := a.token
	a.mu.Unlock()
	if token == "" {
		return 0, "sign in first"
	}
	if p, _, _ := strings.Cut(path, "?"); !launcherPagePath(p, "/viewer") && !launcherPagePath(p, "/fleet") || strings.Contains(path, "..") ||
		(method != http.MethodGet && method != http.MethodPost) {
		return 0, "not allowed"
	}
	req, err := http.NewRequest(method, viewerBaseURL+path, strings.NewReader(body))
	if err != nil {
		return 0, err.Error()
	}
	req.Header.Set("Authorization", "Bearer "+token)
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := launcherHTTPClient().Do(req)
	if err != nil {
		return 0, "server unreachable"
	}
	defer func() { _ = resp.Body.Close() }()
	b, err := io.ReadAll(io.LimitReader(resp.Body, 16<<20)) // terrain runs past 1 MB
	if err != nil {
		return 0, err.Error()
	}
	return resp.StatusCode, string(b)
}

// The Fleet page's 3D view loads the server's public /admin/art (three.js,
// ship meshes, textures: hundreds of MB, too much for the bridge) from this
// loopback proxy; the page cannot fetch the server itself (our own CA).
var artProxy struct {
	once sync.Once
	url  string
}

// ArtBase starts the proxy on first use and returns its base URL ("" if it
// could not listen).
func (a *launcherAPI) ArtBase() string {
	artProxy.once.Do(func() {
		ln, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			return
		}
		artProxy.url = "http://" + ln.Addr().String() + "/art/"
		go func() { _ = http.Serve(ln, http.HandlerFunc(serveArt)) }()
	})
	return artProxy.url
}

func serveArt(w http.ResponseWriter, r *http.Request) {
	p, ok := strings.CutPrefix(r.URL.Path, "/art/")
	if !ok || p == "" || strings.Contains(p, "..") || (r.Method != http.MethodGet && r.Method != http.MethodHead) {
		http.NotFound(w, r)
		return
	}
	resp, err := launcherHTTPClient().Get(strings.TrimSuffix(viewerBaseURL, "/launcher") + "/admin/art/" + p)
	if err != nil {
		http.Error(w, "server unreachable", http.StatusBadGateway)
		return
	}
	defer func() { _ = resp.Body.Close() }()
	for _, h := range []string{"Content-Type", "Content-Length", "Cache-Control", "Last-Modified", "ETag"} {
		if v := resp.Header.Get(h); v != "" {
			w.Header().Set(h, v)
		}
	}
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.WriteHeader(resp.StatusCode)
	_, _ = io.Copy(w, resp.Body)
}

func viewerErr(status int, body string) string {
	if status == 0 {
		return body
	}
	return fmt.Sprintf("HTTP %d", status)
}

// livePingWindow is how many recent round trips the ping shown on the tile is
// the minimum of. It is a "how good is this connection" figure, so a background
// hitch should not be what gets reported.
const livePingWindow = 5

var (
	livePingMu     sync.Mutex
	livePingRecent []int64
	livePingPrimed bool
)

func recordLivePing(ms int64) {
	livePingMu.Lock()
	defer livePingMu.Unlock()
	livePingRecent = append(livePingRecent, ms)
	if len(livePingRecent) > livePingWindow {
		livePingRecent = livePingRecent[len(livePingRecent)-livePingWindow:]
	}
}

// warmLivePing is the minimum of the recent round trips, 0 when none yet.
func warmLivePing() int64 {
	livePingMu.Lock()
	defer livePingMu.Unlock()
	var min int64
	for _, v := range livePingRecent {
		if min == 0 || v < min {
			min = v
		}
	}
	return min
}

// needLivePingPrime reports whether this is the process's first status GET,
// marking it as done.
func needLivePingPrime() bool {
	livePingMu.Lock()
	defer livePingMu.Unlock()
	primed := livePingPrimed
	livePingPrimed = true
	return !primed
}

// Live is the server's mmogbrain/launcher_status.go: players online and the
// battles being fought, plus the signed-in player's account card. Either part
// is left out when the server does not answer it (an older server has
// neither).
func (a *launcherAPI) Live() map[string]any {
	a.mu.Lock()
	token := a.token
	a.mu.Unlock()
	out := map[string]any{}
	var wg sync.WaitGroup
	var mu sync.Mutex
	get := func(key, url, bearer string) {
		defer wg.Done()
		var v map[string]any
		if key == "status" && needLivePingPrime() {
			// The first status GET on a fresh connection also pays the TCP + TLS
			// handshake -- through the relay that is ~245 ms against ~87 ms warm
			// (measured 2026-10-17), which is not what a ping tile should show.
			// One throwaway call before the clock starts, once per process.
			var discard map[string]any
			_ = getJSON(url, bearer, &discard)
		}
		t0 := time.Now()
		if err := getJSON(url, bearer, &v); err == nil {
			mu.Lock()
			out[key] = v
			if key == "status" {
				// Ping: the warm round trip to the server's /launcher/status. The
				// client is cached (launcherHTTPClient), so later refreshes reuse
				// one connection instead of re-handshaking, and the number shown
				// is the smallest of the last few -- a hitch in one refresh must
				// not become the figure on the tile.
				recordLivePing(time.Since(t0).Milliseconds())
				out["ms"] = warmLivePing()
			}
			mu.Unlock()
		}
	}
	wg.Add(1)
	go get("status", liveStatusURL, "")
	if token != "" {
		wg.Add(1)
		go get("me", liveMeURL, token)
	}
	wg.Wait()
	return out
}

// SkipTutorial asks the server to mark the signed-in account's onboarding as
// finished (mmogbrain launcherSkipTutorialHandler); takes effect next login.
func (a *launcherAPI) SkipTutorial() map[string]any {
	a.mu.Lock()
	token := a.token
	a.mu.Unlock()
	if token == "" {
		return map[string]any{"ok": false, "error": "sign in first"}
	}
	var v map[string]any
	if err := doJSON(http.MethodPost, skipTutURL, token, &v); err != nil {
		return map[string]any{"ok": false, "error": err.Error()}
	}
	return map[string]any{"ok": true}
}

func getJSON(url, bearer string, v any) error { return doJSON(http.MethodGet, url, bearer, v) }

func doJSON(method, url, bearer string, v any) error {
	req, err := http.NewRequest(method, url, nil)
	if err != nil {
		return err
	}
	if bearer != "" {
		req.Header.Set("Authorization", "Bearer "+bearer)
	}
	resp, err := launcherHTTPClient().Do(req)
	if err != nil {
		return err
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	return json.NewDecoder(http.MaxBytesReader(nil, resp.Body, 1<<20)).Decode(v)
}
