package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/handlers"
	"github.com/darkace1998/Dreadnought-Revival-project/mmogbrain/protocol"
	"github.com/golang-jwt/jwt/v5"
	"github.com/sirupsen/logrus"
)

// A launcher JWT carries the auth user id, which is a UUID. Anything that is
// not a 32-hex id has to be rejected here: normalizedPlayerStatePID falls back
// to defaultMmogPlayerPID, so an un-normalizable id would key the read/write on
// the template player instead of failing (2026-10-17 audit).
func TestLauncherUIDRejectsNonIDs(t *testing.T) {
	cases := []struct{ header, want string }{
		{"", ""},
		{"not-a-uuid", ""},
		{"dreadnought-user", ""},
		{"00000000-0000-0000-0000-000000000000", ""}, // the zero id is not a player
		{"0123456789abcdef0123456789abcde", ""},      // 31 hex
		{"0123456789abcdef0123456789abcdeff", ""},    // 33 hex
		{"0123456789abcdef0123456789abcdeZ", ""},     // non-hex char
		{"01234567-89ab-cdef-0123-456789abcdef", "0123456789abcdef0123456789abcdef"},
		{"0123456789ABCDEF0123456789ABCDEF", "0123456789abcdef0123456789abcdef"},
	}
	for _, c := range cases {
		r := httptest.NewRequest(http.MethodGet, "/launcher/me", nil)
		if c.header != "" {
			r.Header.Set("X-User-ID", c.header)
		}
		if got := launcherUID(r); got != c.want {
			t.Errorf("launcherUID(X-User-ID: %q) = %q, want %q", c.header, got, c.want)
		}
	}
}

// Both handlers must answer 401 (not 503/501) when the id does not normalize,
// and must not touch the database on that path.
func TestLauncherHandlersRejectBadUserID(t *testing.T) {
	for _, tc := range []struct {
		name   string
		method string
		path   string
		h      http.HandlerFunc
	}{
		{"me", http.MethodGet, "/launcher/me", launcherMeHandler},
		{"skip-tutorial", http.MethodPost, "/launcher/skip-tutorial", launcherSkipTutorialHandler},
	} {
		r := httptest.NewRequest(tc.method, tc.path, nil)
		r.Header.Set("X-User-ID", "not-a-uuid")
		w := httptest.NewRecorder()
		tc.h(w, r)
		if w.Code != http.StatusUnauthorized {
			t.Errorf("%s: status = %d, want 401 (body %q)", tc.name, w.Code, w.Body.String())
		}
	}
}

// The badge the operator reported empty ("-") on 2026-10-17: the launcher's
// sign-in form posts grant_type=password as a plain form, so the auth server
// issues aud="dreadnought" (auth-server handlers.loginPassword), while
// /launcher/me used to demand aud="launcher" alone -- every password sign-in
// 401'd and the account card never loaded. The audiences here are the two the
// auth server can issue; "wrong" stays refused.
func TestLauncherMeAcceptsTheTokenASignInActuallyGets(t *testing.T) {
	useTempMmogPlayerStateDB(t)
	database := currentMmogPlayerStateDB()
	if _, err := database.Exec(`INSERT INTO player_state(user_id, display_name, current_rank, rank_xp)
		VALUES('0123456789abcdef0123456789abcdef','cute-bunny',11,1524)`); err != nil {
		t.Fatalf("seed player_state: %v", err)
	}
	secret := []byte("test-secret")
	log := logrus.New()
	log.SetLevel(logrus.PanicLevel)
	r := newRouter(&handlers.Handler{Log: log}, secret, "admin-key", "internal-key", log)

	get := func(audience string) *httptest.ResponseRecorder {
		tok := mustSignTestJWT(t, jwt.MapClaims{"user_id": "0123456789abcdef0123456789abcdef",
			"username": "cute-bunny", "iss": protocol.GatewayJWTIssuer, "aud": audience,
			"realm": "dreadnought.pc-us", "exp": time.Now().Add(time.Hour).Unix()})
		req := httptest.NewRequest(http.MethodGet, "/launcher/me", nil)
		req.Header.Set("Authorization", "Bearer "+tok)
		rec := httptest.NewRecorder()
		r.ServeHTTP(rec, req)
		return rec
	}

	for _, aud := range []string{"dreadnought", "launcher"} {
		rec := get(aud)
		if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"rank":11`) {
			t.Errorf("aud %q: %d %s; want the account card", aud, rec.Code, rec.Body.String())
		}
	}
	if rec := get("wrong"); rec.Code != http.StatusUnauthorized {
		t.Errorf("aud %q: %d; want 401", "wrong", rec.Code)
	}
}
