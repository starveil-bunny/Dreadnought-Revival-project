//go:build windows

package main

// Where Dreadnought is installed, chosen in the launcher window (Game folder,
// Change...), so the launcher no longer has to live inside the game folder.
// Saved per Windows user in %LOCALAPPDATA%\DreadnoughtPrivateServer\
// settings.json. Lookup order in findGameBinary: game_path from
// dn-launcher.json, this saved folder, the launcher's own folder, Steam.

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"unsafe"

	"golang.org/x/sys/windows"
)

type launcherSettings struct {
	GameDir string `json:"game_dir"`
	// LogWindow opens the game's own log console (-LOG). Off by default: it
	// is a debugging aid, and a second window confuses players.
	LogWindow bool `json:"log_window"`
	// VerboseLog raises the client log to Verbose (see startGame), for bug
	// reports. Much larger log files.
	VerboseLog bool `json:"verbose_log"`
	// LogoFx is the wordmark's border at start ("" off, rgb, aurora, trans,
	// bi, lesbian, custom; clicking the logo cycles and saves them) and
	// LogoCustom the custom gradient's colours, "#rrggbb,#rrggbb[,...]".
	LogoFx     string `json:"logo_fx,omitempty"`
	LogoCustom string `json:"logo_custom,omitempty"`
	// Server is the server picked in the window (savedServer.id; empty =
	// the default) and Servers the ones the player added (servers_windows.go).
	Server  string        `json:"server,omitempty"`
	Servers []savedServer `json:"servers,omitempty"`
	// SetupDone: the setup bunny has run (or was skipped) for this user.
	SetupDone bool `json:"setup_done"`
	// Skin is the launcher page (skins_windows.go); empty = the default.
	Skin string `json:"skin,omitempty"`
}

func settingsPath() string {
	return filepath.Join(os.Getenv("LOCALAPPDATA"), "DreadnoughtPrivateServer", "settings.json")
}

// settingsSet records which keys settings.json actually carries, because a
// bool that is absent and one that is false look identical once unmarshalled
// and the difference matters here: an option the launcher window never touched
// must not override dn-launcher.json or the DN_* environment, while one the
// player turned OFF has to stay off (see startGame's log-window handling).
var settingsSet map[string]bool

func loadSettings() launcherSettings {
	var s launcherSettings
	//nolint:gosec // fixed file under the user's own LOCALAPPDATA.
	data, err := os.ReadFile(settingsPath())
	if err != nil {
		settingsSet = nil
		return s
	}
	var present map[string]json.RawMessage
	if json.Unmarshal(data, &present) == nil {
		settingsSet = make(map[string]bool, len(present))
		for k := range present {
			settingsSet[k] = true
		}
	} else {
		settingsSet = nil
	}
	_ = json.Unmarshal(data, &s)
	return s
}

func saveSettings(s launcherSettings) error {
	if err := os.MkdirAll(filepath.Dir(settingsPath()), 0o700); err != nil {
		return err
	}
	data, _ := json.MarshalIndent(s, "", "  ")
	return os.WriteFile(settingsPath(), data, 0o600)
}

// gameBinaryIn finds the game executable in dir, accepting the install root
// or any folder on the way down to Binaries\Win64 -- players pick whichever
// folder looks right to them.
func gameBinaryIn(dir string) string {
	if strings.TrimSpace(dir) == "" {
		return ""
	}
	win64 := filepath.Join("Binaries", "Win64")
	for _, sub := range []string{
		filepath.Join("DreadGame", "DreadGame", win64),
		filepath.Join("DreadGame", win64),
		win64,
		"Win64",
		"",
	} {
		for _, exe := range []string{"DreadGame-Win64-Shipping-patched.exe", "DreadGame-Win64-Shipping.exe"} {
			p := filepath.Join(dir, sub, exe)
			if _, err := os.Stat(p); err == nil {
				return p
			}
		}
	}
	return ""
}

// gameInstallRoot turns an executable path back into the folder a player
// recognises (the one holding DreadGame\), for display.
func gameInstallRoot(exePath string) string {
	dir := filepath.Dir(exePath)
	suffix := string(filepath.Separator) + filepath.Join("DreadGame", "DreadGame", "Binaries", "Win64")
	if strings.HasSuffix(strings.ToLower(dir), strings.ToLower(suffix)) {
		return dir[:len(dir)-len(suffix)]
	}
	return dir
}

// pickFolder shows Windows' folder picker, owned by the launcher window.
func pickFolder(owner uintptr, title string) (string, bool) {
	shell32 := windows.NewLazySystemDLL("shell32.dll")
	const (
		bifReturnOnlyFSDirs  = 0x0001
		bifNewDialogStyle    = 0x0040
		bifNoNewFolderButton = 0x0200
	)
	var display [windows.MAX_PATH]uint16
	t, _ := windows.UTF16PtrFromString(title)
	bi := struct {
		Owner       uintptr
		Root        uintptr
		DisplayName *uint16
		Title       *uint16
		Flags       uint32
		Callback    uintptr
		LParam      uintptr
		Image       int32
	}{Owner: owner, DisplayName: &display[0], Title: t,
		Flags: bifReturnOnlyFSDirs | bifNewDialogStyle | bifNoNewFolderButton}
	pidl, _, _ := shell32.NewProc("SHBrowseForFolderW").Call(uintptr(unsafe.Pointer(&bi)))
	if pidl == 0 {
		return "", false // cancelled
	}
	defer func() { _, _, _ = windows.NewLazySystemDLL("ole32.dll").NewProc("CoTaskMemFree").Call(pidl) }()
	var path [windows.MAX_PATH]uint16
	if r, _, _ := shell32.NewProc("SHGetPathFromIDListW").Call(pidl, uintptr(unsafe.Pointer(&path[0]))); r == 0 {
		return "", false
	}
	return windows.UTF16ToString(path[:]), true
}
