#!/usr/bin/env bash
# setup.sh -- one-shot setup for the Dreadnought private server on Linux.
#
# Builds every service, generates TLS certificates, and writes a run/secrets.env
# with fresh random keys. Safe to re-run: it never overwrites an existing
# certificate or secrets file.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RUN_DIR="$PROJECT_DIR/run"

echo "================================================"
echo "  Dreadnought private server -- setup"
echo "================================================"
echo

# ---------------------------------------------------------------- prerequisites
if ! command -v go &>/dev/null; then
  echo "[!] Go not found. Install Go 1.24+ from https://golang.org/dl/" >&2
  exit 1
fi
GO_VERSION="$(go version | awk '{print $3}' | sed 's/^go//')"
GO_MAJOR="${GO_VERSION%%.*}"
GO_MINOR="$(echo "$GO_VERSION" | cut -d. -f2)"
if [ "$GO_MAJOR" -lt 1 ] || { [ "$GO_MAJOR" -eq 1 ] && [ "$GO_MINOR" -lt 24 ]; }; then
  # go.work declares go 1.25.0 and the modules declare 1.24.0. An older
  # toolchain fails with a confusing "go.mod requires go >= ..." per module.
  echo "[!] Go $GO_VERSION is too old; the workspace needs 1.24+." >&2
  exit 1
fi
echo "[OK] Go $GO_VERSION"

if ! command -v openssl &>/dev/null; then
  echo "[!] openssl not found. Install with: apt install openssl" >&2
  exit 1
fi
echo "[OK] openssl"

if command -v wine &>/dev/null; then
  echo "[OK] wine $(wine --version 2>/dev/null || echo '?')"
else
  # Only game-manager needs it, and only to spawn battle servers. Everything up
  # to and including the hangar works without Wine.
  echo "[--] wine not found -- matchmaking will form matches but no battle"
  echo "     server can be spawned. Install wine (or wine-staging) for that."
fi

# ---------------------------------------------------------------- certificates
if [ -f "$PROJECT_DIR/certs/server.crt" ]; then
  echo "[OK] TLS certificates already exist (delete certs/ to regenerate)"
else
  echo
  echo "[*] Generating TLS certificates..."
  bash "$SCRIPT_DIR/gen-certs.sh"
fi

# ---------------------------------------------------------------------- build
echo
echo "[*] Building services..."
mkdir -p "$RUN_DIR"

SERVICES=(auth-server legacy-api mmogbrain master-server game-manager gateway admin-cli)
for svc in "${SERVICES[@]}"; do
  printf '    %-16s ' "$svc"
  (cd "$PROJECT_DIR" && go build -o "$RUN_DIR/$svc" "./$svc")
  echo "OK"
done

# dn-dedicated is the control plane that replaced game-manager (see
# start-services.sh). It is a SEPARATE Go module and is deliberately not in
# go.work, so it builds with GOWORK=off from its own directory.
printf '    %-16s ' "dn-dedicated"
if (cd "$PROJECT_DIR/dn-dedicated" && GOWORK=off go build -o "$RUN_DIR/dn-dedicated" .); then
  echo "OK"
else
  echo "FAILED -- the stack will fall back to game-manager"
fi

# The launcher is the one client-side component, and it is Windows-only
# (//go:build windows). Cross-compiling it here is optional -- skip quietly if
# the toolchain cannot.
printf '    %-16s ' "dn-launcher.exe"
if (cd "$PROJECT_DIR" && GOOS=windows GOARCH=amd64 go build -o "$RUN_DIR/dn-launcher.exe" ./dn-launcher) 2>/dev/null; then
  echo "OK (windows/amd64)"
else
  echo "skipped (cross-compile unavailable)"
fi

# ---------------------------------------------------------------------- secrets
echo
if [ -f "$RUN_DIR/secrets.env" ]; then
  echo "[OK] run/secrets.env already exists (left untouched)"
else
  echo "[*] Generating run/secrets.env with fresh random keys..."
  JWT="$(openssl rand -hex 32)"
  ADMIN="$(openssl rand -hex 32)"
  sed -e "s|^JWT_SECRET=$|JWT_SECRET=$JWT|" \
      -e "s|^ADMIN_KEY=$|ADMIN_KEY=$ADMIN|" \
      "$SCRIPT_DIR/secrets.env.example" > "$RUN_DIR/secrets.env"
  chmod 600 "$RUN_DIR/secrets.env"
  echo "[OK] run/secrets.env written (mode 600, gitignored)"
fi

# ------------------------------------------------------------------ game data
# The server reads the game's extracted item tables from data/. They are
# committed, so a clone has them; a missing data/ means every id-to-name lookup
# silently falls back and the client gets nonsense.
if [ -d "$PROJECT_DIR/data/assets" ]; then
  echo "[OK] game data present ($(find "$PROJECT_DIR/data" -type f | wc -l) files under data/)"
else
  echo "[!] data/assets is missing -- item names and ids will not resolve." >&2
fi

# ------------------------------------------------------------------ admin art
# The admin dashboard's and launcher's pictures, ship stats and texts are not
# committed (client art); scripts/gen-admin-art.py extracts them from the game
# install. Needs python3 + Pillow and the .NET SDK (tools/bpdump, which builds
# against UAssetAPI cloned beside this repo). Point DN_PAKS
# at DreadGame/DreadGame/Content/Paks if the game is not in the Steam default.
PAKS="${DN_PAKS:-$HOME/.local/share/Steam/steamapps/common/Dreadnought/DreadGame/DreadGame/Content/Paks}"
if [ -f "$PROJECT_DIR/data/admin-art/art.json" ]; then
  echo "[OK] admin art present"
elif [ -d "$PAKS" ] && command -v dotnet >/dev/null && python3 -c "import PIL" 2>/dev/null; then
  echo "[*] extracting admin art from $PAKS (a few minutes)"
  ART_TMP="$(mktemp -d)"
  UASSET="$PROJECT_DIR/../UAssetAPI"   # tools/bpdump/bpdump.csproj references it there
  [ -d "$UASSET" ] || { git clone -q https://github.com/atenfyr/UAssetAPI "$UASSET" &&
                        git -C "$UASSET" checkout -q 33ef77e; }
  if dotnet build -v q "$PROJECT_DIR/tools/bpdump/bpdump.csproj" >/dev/null &&
     DN_PAKS="$PAKS" DN_ART_CONTENT="$ART_TMP/DreadGame/Content" \
       python3 "$PROJECT_DIR/scripts/gen-admin-art.py" --extract >/dev/null; then
    echo "[OK] admin art extracted"
  else
    echo "[!] admin art extraction failed -- the dashboard and launcher show no pictures" >&2
  fi
  rm -rf "$ART_TMP"
else
  echo "[!] no admin art: needs the game (DN_PAKS), dotnet and python3-pil; then re-run setup" >&2
fi

# --------------------------------------------------------------- market images
# The store's ship and cosmetic pictures (data/market-images/) are client art
# too, decoded from the paks. Needs python3 + Pillow.
if ls "$PROJECT_DIR"/data/market-images/*.png >/dev/null 2>&1; then
  echo "[OK] market images present"
elif [ -d "$PAKS" ] && python3 -c "import PIL" 2>/dev/null; then
  echo "[*] extracting market images from $PAKS"
  if DN_PAKS="$PAKS" python3 "$PROJECT_DIR/scripts/gen-hero-market-data.py" >/dev/null &&
     DN_PAKS="$PAKS" python3 "$PROJECT_DIR/scripts/gen-vanity-icons.py" >/dev/null; then
    echo "[OK] market images extracted"
  else
    echo "[!] market image extraction failed -- store tiles show no pictures" >&2
  fi
else
  echo "[!] no market images: needs the game (DN_PAKS) and python3-pil; then re-run setup" >&2
fi

echo
echo "================================================"
echo "  Setup complete"
echo "================================================"
echo
echo "  Binaries:      $RUN_DIR/"
echo "  Certificates:  $PROJECT_DIR/certs/"
echo "  Secrets:       $RUN_DIR/secrets.env"
echo
echo "  Next:"
echo "    1. Review run/secrets.env (set GAME_BINARY to run battle servers)."
echo "    2. Start the stack:   bash scripts/start-services.sh"
echo "    3. On each client:    install certs/ca.crt as a trusted root CA and"
echo "                          redirect the Greybox hostnames to this server"
echo "                          (scripts/hosts-redirect.sh / .ps1)."
echo
echo "  Stop the stack with:    bash scripts/stop-services.sh"
echo "  Admin CLI:              ADMIN_KEY=<key> $RUN_DIR/admin-cli help"
echo
