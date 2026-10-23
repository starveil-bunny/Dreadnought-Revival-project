package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"net/http"
)

// Cosmetic removal for the admin dashboard (#70, the operator side of #69).
// The game has no way to drop an owned item: its only removal request,
// YA_RemoveItems, is reachable only from the developer console and refuses ids
// of 20M and up (#69). So the operator deletes the player_purchases row here.
// Owned items reach the client only in the login player data, so a removal
// shows in game at the player's next login, online or not. Only cosmetics
// (item_type 'vanity') can be removed: ships, modules and research rows carry
// progression and loadouts that other tables point at. Nothing is refunded;
// use the balance form if the player should get the price back.

type adminItem struct {
	ID          int32  `json:"id"`
	Hex         string `json:"hex"`
	Name        string `json:"name"`
	Price       int32  `json:"price"`
	Currency    string `json:"currency"`
	PurchasedAt string `json:"purchased_at"`
}

func adminPlayerCosmetics(database *sql.DB, pid string) []adminItem {
	out := []adminItem{}
	rows, err := database.Query(`SELECT item_id, price_paid, currency, purchased_at FROM player_purchases
		WHERE user_id=? AND item_type='vanity' ORDER BY purchased_at DESC, item_id`, pid)
	if err != nil {
		return out
	}
	defer func() { _ = rows.Close() }()
	for rows.Next() {
		var it adminItem
		if rows.Scan(&it.ID, &it.Price, &it.Currency, &it.PurchasedAt) != nil {
			continue
		}
		it.Hex = fmt.Sprintf("0x%08X", uint32(it.ID))
		if name, ok := authoritativeItemName(it.ID); ok {
			it.Name = name
		}
		out = append(out, it)
	}
	return out
}

// adminAPIPlayerRemoveItem deletes one owned cosmetic: body {"item_id": n}.
func adminAPIPlayerRemoveItem(w http.ResponseWriter, r *http.Request) {
	pid, database, ok := adminPlayerPID(w, r)
	if !ok {
		return
	}
	var req struct {
		ItemID *int32 `json:"item_id"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&req); err != nil || req.ItemID == nil {
		http.Error(w, `{"error":"item_id required"}`, http.StatusBadRequest)
		return
	}
	var price int32
	var currency string
	err := database.QueryRow(`SELECT price_paid, currency FROM player_purchases WHERE user_id=? AND item_id=? AND item_type='vanity'`,
		pid, *req.ItemID).Scan(&price, &currency)
	if err == sql.ErrNoRows {
		http.Error(w, `{"error":"the player does not own that cosmetic"}`, http.StatusNotFound)
		return
	} else if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if _, err := database.Exec(`DELETE FROM player_purchases WHERE user_id=? AND item_id=? AND item_type='vanity'`, pid, *req.ItemID); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	name, _ := authoritativeItemName(*req.ItemID)
	adminAudit(database, r, "remove cosmetic", pid, map[string]any{"item_id": *req.ItemID,
		"hex": fmt.Sprintf("0x%08X", uint32(*req.ItemID)), "name": name, "price_paid": price, "currency": currency})
	adminAPIPlayerDetail(w, r)
}
