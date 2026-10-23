//go:build windows

package main

import (
	"crypto/x509"
	"testing"
)

// The tile must not show a handshake. The client used to be rebuilt per
// request, so every call paid TCP + TLS (~245 ms through the relay against
// ~87 ms warm, measured 2026-10-17).
func TestLauncherHTTPClientIsReusedUntilTheServerChanges(t *testing.T) {
	oldIP, oldPort, oldPool := serverDialIP, serverWebPort, serverCAPool
	defer func() { serverDialIP, serverWebPort, serverCAPool = oldIP, oldPort, oldPool }()

	serverDialIP, serverWebPort, serverCAPool = "203.0.113.9", "", nil
	first := launcherHTTPClient()
	if got := launcherHTTPClient(); got != first {
		t.Error("a second call built another client: every request would re-handshake")
	}

	// Each field the transport reads must invalidate the cache, or the launcher
	// keeps talking to the previous server after UseServer.
	serverDialIP = "203.0.113.10"
	if got := launcherHTTPClient(); got == first {
		t.Error("a new dial IP kept the old client")
	}
	afterIP := launcherHTTPClient()
	serverWebPort = "8443"
	if got := launcherHTTPClient(); got == afterIP {
		t.Error("a new web port kept the old client")
	}
	afterPort := launcherHTTPClient()
	serverCAPool = x509.NewCertPool() // only its identity matters to the cache key
	if got := launcherHTTPClient(); got == afterPort {
		t.Error("a new CA pool kept the old client")
	}
	// And it settles again once the settings stop moving.
	stable := launcherHTTPClient()
	if got := launcherHTTPClient(); got != stable {
		t.Error("the rebuilt client was not cached")
	}
}

func resetLivePing() {
	livePingMu.Lock()
	livePingRecent, livePingPrimed = nil, false
	livePingMu.Unlock()
}

func TestWarmLivePingIsTheBestOfTheWindow(t *testing.T) {
	resetLivePing()
	defer resetLivePing()

	if got := warmLivePing(); got != 0 {
		t.Fatalf("with no samples the ping is %d, want 0", got)
	}
	recordLivePing(242) // the cold first call
	if got := warmLivePing(); got != 242 {
		t.Fatalf("one sample: %d, want 242", got)
	}
	recordLivePing(88)
	recordLivePing(95)
	if got := warmLivePing(); got != 88 {
		t.Errorf("ping %d, want the warm 88 rather than the cold 242 or the latest", got)
	}
	for i := 0; i < livePingWindow+3; i++ {
		recordLivePing(300)
	}
	if got := warmLivePing(); got != 300 {
		t.Errorf("ping %d, want the window to have forgotten both old low samples", got)
	}

	if !needLivePingPrime() {
		t.Error("the first status GET must prime the connection")
	}
	if needLivePingPrime() {
		t.Error("only the first status GET may prime")
	}
}
