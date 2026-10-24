package main

import (
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	dreadconfig "github.com/darkace1998/Dreadnought-Revival-project/shared/dreadgameconfig"
	"github.com/sirupsen/logrus"
)

// Market pictures: the hero icons the client ships as cooked textures,
// decoded to PNG by scripts/gen-hero-market-data.py. The original store served
// its pictures from a CDN by URL, and the client still only takes URLs
// (full_image_url and friends), so the gateway serves them itself.

const marketImagePath = "/market-images/"

// marketImageName admits only the generator's file names, so a request can
// never leave the directory.
var marketImageName = regexp.MustCompile(`^[A-Za-z0-9_-]+\.png$`)

func handleMarketImage(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, marketImagePath)
	if r.Method != http.MethodGet && r.Method != http.MethodHead || !marketImageName.MatchString(name) {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", "public, max-age=86400")
	if !strings.HasPrefix(r.RemoteAddr, "127.") {
		logrus.WithFields(logrus.Fields{"name": name, "host": r.Host, "by": r.RemoteAddr}).Info("market image")
	}
	http.ServeFile(w, r, marketImageFile(name))
}

// marketImageFile: with DN_BUNNY_IMAGES=1 the bunny photo of the same name from
// data/market-images-bunny (scripts/bunny-photo-market-images.py) when it exists.
// Same file names, so the catalog does not change; off by default.
func marketImageFile(name string) string {
	if os.Getenv("DN_BUNNY_IMAGES") == "1" {
		bunny := filepath.Join(filepath.Dir(dreadconfig.MarketImagesDir()), "market-images-bunny", name)
		if _, err := os.Stat(bunny); err == nil {
			return bunny
		}
	}
	return filepath.Join(dreadconfig.MarketImagesDir(), name)
}

// gatewayAbsoluteImageURLs makes the catalog's picture paths absolute on the
// host and scheme the client fetched the catalog from -- the one origin it is
// known to reach and to trust.
func gatewayAbsoluteImageURLs(payload map[string]any, r *http.Request) map[string]any {
	scheme := "https"
	if r.TLS == nil {
		scheme = "http"
	}
	base := scheme + "://" + marketImageHost(r.Host)
	for _, list := range payload {
		entities, ok := list.([]any)
		if !ok {
			continue
		}
		for _, e := range entities {
			entity, ok := e.(map[string]any)
			if !ok {
				continue
			}
			for key, v := range entity {
				if url, ok := v.(string); ok && strings.HasPrefix(url, marketImagePath) {
					entity[key] = base + url
				}
			}
		}
	}
	return payload
}

// marketImageHost: the catalog's Host, except a public IP (a relayed friend's
// client reaches the gateway at the relay's edge IP, from -GatewayAddress)
// becomes SERVER_IP (PUBLIC_HOST) on the same port. The server certificate
// names the DNS name but not the relay IP, so picture fetches by IP failed
// TLS (2026-10-23: shop bunnies did not load for a relayed friend; the API
// itself is reached the same way, so it does not check the name).
// GUESS: the picture loader verifies the name while the API client does not.
func marketImageHost(host string) string {
	h, port, err := net.SplitHostPort(host)
	if err != nil {
		h, port = host, ""
	}
	ip := net.ParseIP(h)
	name := os.Getenv("SERVER_IP")
	if ip == nil || ip.IsLoopback() || ip.IsPrivate() || name == "" || net.ParseIP(name) != nil {
		return host
	}
	if port == "" {
		return name
	}
	return net.JoinHostPort(name, port)
}
