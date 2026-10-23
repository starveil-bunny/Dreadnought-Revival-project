package dreadgameconfig

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"sync"
)

// Captain templates and cosmetic texts, from the client's own data
// (scripts/gen-hero-market-data.py writes both).
//
// CaptainTemplates.json: the captain items each gender's template
// (Captain_Template_<Male|Female>_01, a YCharacterTemplate: m_meshes,
// m_materials) lets a captain wear. Items in neither template -- the NPC
// officers' and traders' heads, the male C00a/C00b/C01 heads, the seasonal
// "_Outfit" variants, Outfit_Base_Military, test hair -- showed up in the
// captain editor once owned, as options with no head, no picture, or a suit
// tab with nothing in it (operator, 2026-10-06).
//
// VanityTexts.json: each cosmetic's name, subline and description in every
// language the client ships, from its m_itemUIData keys -- what the Market
// needs as locale objects (see HeroMarketData).

type vanityText struct {
	Name        Localized `json:"name"`
	Subline     Localized `json:"subline"`
	Description Localized `json:"description"`
}

var (
	vanityExtrasOnce sync.Once
	captainTemplate  map[int32]bool
	vanityTexts      map[string]vanityText
)

func loadVanityExtras() {
	captainTemplate = map[int32]bool{}
	var templates map[string][]int32
	if raw, err := os.ReadFile(filepath.Join(DataDir(), "vanity", "CaptainTemplates.json")); err == nil {
		_ = json.Unmarshal(raw, &templates)
	}
	for _, ids := range templates {
		for _, id := range ids {
			captainTemplate[id] = true
		}
	}
	if raw, err := os.ReadFile(filepath.Join(DataDir(), "vanity", "VanityTexts.json")); err == nil {
		_ = json.Unmarshal(raw, &vanityTexts)
	}
}

// InCaptainTemplate reports whether some gender's captain template lists the
// item. False for every item when the template data is missing.
func InCaptainTemplate(itemID int32) bool {
	vanityExtrasOnce.Do(loadVanityExtras)
	return captainTemplate[itemID]
}

// captainTemplatesLoaded reports whether the template data was found.
func captainTemplatesLoaded() bool {
	vanityExtrasOnce.Do(loadVanityExtras)
	return len(captainTemplate) > 0
}

// VanityItemText is a cosmetic's localized name and description. The
// description falls back to the item's subline, then its name: the Market's
// offer converter (0x142A7D7E0, called from 0x142A5B960) reports "<DNT>
// Invalid Description Field in Json" for any description shorter than two
// characters, and 240 named cosmetics have no English description.
func VanityItemText(itemID int32) (name, description Localized, ok bool) {
	vanityExtrasOnce.Do(loadVanityExtras)
	t, ok := vanityTexts[strconv.Itoa(int(itemID))]
	if n, found := vanityNameNotInLocres[itemID]; found && t.Name["en"] == "" {
		t.Name, t.Description, ok = Localized{"en": n}, Localized{"en": ds3Description}, true
	}
	description = t.Description
	for _, fallback := range []Localized{t.Subline, t.Name} {
		if description["en"] != "" {
			break
		}
		description = fallback
	}
	return t.Name, description, ok
}

// vanityNameNotInLocres names cosmetics whose headline keys are in no shipped
// .locres (all five en tables searched 2026-10-21): the "Deep Space Explorers"
// outfit, from Dreadnought's Steam news post of 2021-04-23 ("NEW OUTFIT: DEEP
// SPACE SURVIVAL SUIT, HELMET, AND CAPTAIN TINT"). English only. The (M)/(F)
// suffixes follow the other captain items' names.
var vanityNameNotInLocres = map[int32]string{
	872349948: "Deep Space Survival Suit (F)",   // Body_Female_ExplorerSet
	872349949: "Deep Space Survival Suit (M)",   // Body_Male_ExplorerSet
	872349950: "Deep Space Survival Helmet (M)", // CH_Attachment_ExplorerSet
	872349951: "Deep Space Survival Helmet (F)", // CH_Attachment_ExplorerSet_F
	855572547: "Deep Space Captain Tint",        // Mat_Tint_Explorer
}

// ds3Description is the same post's text for the outfit.
const ds3Description = "Dating back to the Golden Age of Space Exploration, the Deep Space Survival Suit (or DS3) was critical in protecting the first human explorers as they traveled into the far reaches of the Solar System. After an eruption on Ceres revealed a large stockpile of DS3s, it has since been re-embraced for its elegance and durability."
