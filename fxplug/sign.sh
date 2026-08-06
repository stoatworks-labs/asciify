#!/usr/bin/env bash
# Sign the FxPlug wrapper app and everything nested inside it.
#
# Signing is NOT optional here the way it is for the FFGL and OpenFX bundles: an
# unsigned FxPlug plug-in is refused by the system and never appears in Final Cut
# Pro or Motion at all. docs/UNSIGNED.md does not apply to this artefact.
#
# Order matters. codesign will not re-sign a bundle whose contents change
# afterwards, so the nested pieces are signed first and the app last.
#
# Usage: fxplug/sign.sh "path/to/Stoatworks Luma Key.app" [identity]

set -euo pipefail

APP="${1:?usage: sign.sh <app> [identity]}"
IDENTITY="${2:-Developer ID Application: ALLAN SARGEANT (3G7USP8N73)}"

if [ ! -d "$APP" ]; then
	echo "error: no app at $APP" >&2
	exit 1
fi

sign() {
	codesign --force --timestamp --options runtime --sign "$IDENTITY" "$@"
}

echo "==> embedded frameworks"
for fw in "$APP/Contents/Frameworks/"*.framework; do
	[ -e "$fw" ] || continue
	# Apple ships these signed by Apple; re-signing with our identity is what the
	# Xcode template's own build phase does, and is required because the
	# containing app's signature must cover them.
	sign "$fw"
done

echo "==> XPC service"
for svc in "$APP/Contents/PlugIns/"*.pluginkit; do
	[ -e "$svc" ] || continue
	sign "$svc"
done

echo "==> wrapper app"
sign "$APP"

echo "==> verify"
codesign --verify --deep --strict --verbose=2 "$APP"
