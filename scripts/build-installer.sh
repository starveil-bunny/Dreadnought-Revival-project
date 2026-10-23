#!/usr/bin/env bash
# Builds run/dn-install.exe (NSIS wizard): dn-launcher.exe + certs/ca.crt
# (+ optional dn-launcher.json) inside.
#   SERVER=play.example.org [WEB_PORT=8443] [CONFIG=path/dn-launcher.json] scripts/build-installer.sh
# makensis from PATH, else NSIS_DIR/makensis.exe under wine (NSIS zip from nsis.sourceforge.io).
set -euo pipefail
cd "$(dirname "$0")/.."
P=dn-launcher/installer/payload
find "$P" -mindepth 1 ! -name .keep ! -name .gitignore -delete
LD="-H windowsgui"
[ -n "${SERVER:-}" ] && LD+=" -X main.defaultServer=$SERVER"
[ -n "${WEB_PORT:-}" ] && LD+=" -X main.defaultWebPort=$WEB_PORT"
[ -f certs/ca.crt ] && LD+=" -X main.defaultCA=$(base64 -w0 certs/ca.crt)" && cp certs/ca.crt "$P/"
[ -n "${CONFIG:-}" ] && cp "$CONFIG" "$P/dn-launcher.json"
(cd dn-launcher && GOOS=windows GOARCH=amd64 go build -ldflags "$LD" -o installer/payload/dn-launcher.exe .)
mkdir -p run
cd dn-launcher/installer
if command -v makensis >/dev/null; then
  makensis -V2 dn-install.nsi
else
  WINEDEBUG=-all wine "${NSIS_DIR:-$HOME/.cache/nsis-dl/nsis-3.10}/makensis.exe" /V2 dn-install.nsi
fi
ls -l ../../run/dn-install.exe
