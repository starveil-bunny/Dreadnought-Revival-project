package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/sirupsen/logrus"
)

// Two different listeners share the name "gateway": the service on :443 that
// proxies by Host header, and GATEWAY_ADDR (:65443) inside mmogbrain, which is
// the game's Greybox web services. /launcher/* is served by the former
// (dn-launcher dials https://mmog.greybox.sixfoot.live/launcher/...). On the
// Greybox socket the catch-all used to answer 200 {} for anything unmatched,
// so an unauthenticated POST /launcher/skip-tutorial looked like a success
// there while the real listener answers 401 (2026-10-17 audit).
func TestGreyboxSocketRejectsLauncherPaths(t *testing.T) {
	log := logrus.New()
	log.SetOutput(io.Discard)
	h := gatewayUnknownHandler(log)

	for _, path := range []string{"/launcher/skip-tutorial", "/launcher/me", "/launcher/status"} {
		r := httptest.NewRequest(http.MethodPost, path, nil)
		w := httptest.NewRecorder()
		h(w, r)
		if w.Code != http.StatusNotFound {
			t.Errorf("%s: status = %d, want 404 (body %q)", path, w.Code, w.Body.String())
		}
	}

	// Everything else keeps the empty-200 contract the game's probes rely on.
	r := httptest.NewRequest(http.MethodPost, "/api/v1/not-modeled", nil)
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusOK || w.Body.String() != "{}\n" {
		t.Errorf("unmodeled path: status = %d body %q, want 200 {}", w.Code, w.Body.String())
	}
	if ct := w.Header().Get("Content-Type"); ct != "application/json" {
		t.Errorf("unmodeled path: content-type = %q, want application/json", ct)
	}
}
