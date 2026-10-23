package main

import (
	"encoding/json"
	"net/http"
	"testing"
)

// The admin removes an owned cosmetic; other purchase kinds stay untouchable.
func TestAdminRemovesOnlyCosmetics(t *testing.T) {
	database := useTempMmogPlayerStateDB(t)
	if err := seedMmogPlayerState(database, adminTestPID); err != nil {
		t.Fatal(err)
	}
	const cosmetic, module = int32(0x16FF0002), int32(0x05000001)
	for _, q := range []struct {
		id  int32
		typ string
	}{{cosmetic, "vanity"}, {module, "module"}} {
		if _, err := database.Exec(`INSERT INTO player_purchases(user_id,item_id,item_type,price_paid,currency) VALUES(?,?,?,100,'gp')`,
			adminTestPID, q.id, q.typ); err != nil {
			t.Fatal(err)
		}
	}
	r := adminTestRouter()
	path := "/admin/api/players/" + adminTestPID + "/items/remove"
	if rec := adminPost(r, path, "", `{"item_id":1}`); rec.Code != http.StatusForbidden {
		t.Fatalf("no key: %d", rec.Code)
	}
	detail := adminGet(r, "/admin/api/players/"+adminTestPID, "test-admin-key")
	var d struct {
		Cosmetics []adminItem `json:"cosmetics"`
	}
	if err := json.Unmarshal(detail.Body.Bytes(), &d); err != nil || len(d.Cosmetics) != 1 || d.Cosmetics[0].Hex != "0x16FF0002" {
		t.Fatalf("detail cosmetics: %v %+v", err, d.Cosmetics)
	}
	if rec := adminPost(r, path, "test-admin-key", `{"item_id":83886081}`); rec.Code != http.StatusNotFound {
		t.Errorf("module removal: %d %s", rec.Code, rec.Body.String())
	}
	if rec := adminPost(r, path, "test-admin-key", `{"item_id":385810434}`); rec.Code != http.StatusOK {
		t.Fatalf("cosmetic removal: %d %s", rec.Code, rec.Body.String())
	}
	var n int
	_ = database.QueryRow(`SELECT COUNT(*) FROM player_purchases WHERE user_id=?`, adminTestPID).Scan(&n)
	var left int32
	_ = database.QueryRow(`SELECT item_id FROM player_purchases WHERE user_id=?`, adminTestPID).Scan(&left)
	if n != 1 || left != module {
		t.Errorf("after removal: %d rows, left %#x", n, left)
	}
	if rec := adminPost(r, path, "test-admin-key", `{}`); rec.Code != http.StatusBadRequest {
		t.Errorf("empty body: %d", rec.Code)
	}
}
