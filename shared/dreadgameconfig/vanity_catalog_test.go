package dreadgameconfig

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestVanityCatalogFromTheClientsAssets(t *testing.T) {
	items := VanityItems()
	if len(items) < 1400 {
		t.Fatalf("%d vanity items, the cooked dump has 1494", len(items))
	}
	sold, free := 0, 0
	for _, v := range items {
		if VanityItemIsSold(v) {
			sold++
			if VanityItemIsFree(v) {
				free++
			}
		}
	}
	t.Logf("%d cosmetics, %d sold, %d of them free", len(items), sold, free)
	// Every named, non-test item is sold (2026-09-28: 1,465 of the 1,494),
	// except captain meshes no captain template lists (2026-10-06: 38 more
	// out -- the male C00a/C00b/C01 heads, the seasonal "_Outfit" variants,
	// the Explorer sets, Outfit_Base_Military, 44 head attachments...).
	// 2026-10-21: 1427 -> 1449, the 22 later captain meshes (laterCaptainMesh).
	if sold < 1420 || sold > 1460 {
		t.Errorf("%d sold, want the named, non-test, wearable items (~1449)", sold)
	}
	// A set piece the templates list is sold; a later set they do not list is
	// too, but not a head or an _Outfit duplicate of a listed set.
	for name, wantSold := range map[string]bool{"Body_Female_AutumnSet": true, "Body_Female_ExplorerSet": true,
		"Head_Male_C00a": false, "Body_Female_SpringSet_Outfit": false, "Body_Male_NanoDocSet_Outfit": true} {
		found := false
		for _, v := range items {
			if v.Name == name {
				found = true
				if VanityItemIsSold(v) != wantSold {
					t.Errorf("%s sold=%v, want %v", name, VanityItemIsSold(v), wantSold)
				}
			}
		}
		if !found {
			t.Errorf("%s missing from the cooked dump", name)
		}
	}
	for _, v := range items {
		if VanityItemIsSold(v) && (v.HeadlineKey == "" || strings.HasSuffix(strings.ToUpper(v.Name), "_TEST")) {
			t.Errorf("%s is sold but is a test item or has no name", v.Name)
		}
	}
	// The pieces of the captain the client saved live (2026-09-24) are free.
	// Body_Male_Base_MC01 was one of them, but no template lists it, and owned
	// (2026-10-06) it made a captain-editor suit tab with nothing in it; it is
	// still free, not sold, and not granted (see VanityItemIsPlayerFacing).
	if v, ok := VanityItemByID(872349903); !ok || !VanityItemIsFree(v) || VanityItemIsSold(v) {
		t.Errorf("Body_Male_Base_MC01: known=%v free=%v sold=%v, want free and not sold", ok, ok && VanityItemIsFree(v), ok && VanityItemIsSold(v))
	}
	for _, id := range []int32{855572482 /*Mat_Eyes_Default*/, 872349840 /*Head_Male_B03*/} {
		v, ok := VanityItemByID(id)
		if !ok || !VanityItemIsFree(v) || !VanityItemIsSold(v) {
			t.Errorf("item %d (%s): known=%v free=%v sold=%v", id, v.Name, ok, ok && VanityItemIsFree(v), ok && VanityItemIsSold(v))
		}
	}
	// A plain emblem is sold, not free.
	if v, ok := VanityItemByID(369033217); !ok || VanityItemIsFree(v) || !VanityItemIsSold(v) {
		t.Errorf("VAN_EMB_Bear should be sold and not free")
	}
	// NPC heads and the gender items have no player-facing name: not sold.
	for _, id := range []int32{872349768 /*Head_ChiefOfficer*/, 889126914 /*CH_Gender_Male*/} {
		if v, ok := VanityItemByID(id); ok && VanityItemIsSold(v) {
			t.Errorf("%s is sold but has no player-facing name", v.Name)
		}
	}
	if v, ok := VanityItemByID(369033217); ok && v.HeadlineKey == "" {
		t.Error("VAN_EMB_Bear has no headline localization key")
	}
}

// A cosmetic's picture is the texture its own cooked data names, and only when
// that texture really decodes: scripts/gen-vanity-icons.py writes the manifest
// from the paks, and an item whose icon is a material gets no entry rather than
// a substitute (nothing is invented).
func TestVanityIconsComeFromTheClientsOwnTexture(t *testing.T) {
	isolation, ok := VanityItemByID(385810556) // "Isolation", a coating
	if !ok {
		t.Fatal("385810556 (Isolation) missing from the cooked dump")
	}
	if isolation.IconPath == "" || isolation.IconHighResPath == "" {
		t.Errorf("Isolation carries no icon path: %q / %q, so the manifest cannot be derived",
			isolation.IconPath, isolation.IconHighResPath)
	}
	// Its m_icon is the SHARED coating glyph, which is the water-drop the
	// operator sees on that tile -- the client's own choice for these items,
	// not a placeholder we introduced.
	if got := VanityIconFile(385810556); got != "UI_Vanity_Coating.png" {
		t.Errorf("Isolation icon = %q, want UI_Vanity_Coating.png", got)
	}
	// A decal whose m_icon is the item's own material under VanityItems/, not a
	// Texture2D: no picture.
	if got := VanityIconFile(419365113); got != "" {
		t.Errorf("Venomous decal icon = %q, want none (its m_icon is not a texture)", got)
	}
	withIcon, without := 0, 0
	for _, v := range VanityItems() {
		file := VanityIconFile(v.ItemID)
		if v.IconPath == "" && file != "" {
			t.Errorf("%s has no m_icon but is served %q", v.Name, file)
		}
		if file == "" {
			without++
			continue
		}
		withIcon++
		if _, err := os.Stat(filepath.Join(DataDir(), "market-images", file)); err != nil {
			t.Errorf("%s (%d) is served %q but that file is not in data/market-images", v.Name, v.ItemID, file)
		}
	}
	t.Logf("%d cosmetics have a picture, %d do not", withIcon, without)
	if withIcon < 1100 {
		t.Errorf("only %d cosmetics have a picture; the paks carry one for far more", withIcon)
	}
}
