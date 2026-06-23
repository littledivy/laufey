#!/usr/bin/env bash
# Sign a CMake-built iOS .app and package it into an .ipa.
#
#   ios/package-ipa.sh <app> <bundle-id> <signing-identity> <provisioning-profile> [out.ipa]
#
# Example:
#   ios/package-ipa.sh webview/build-ios/laufey_webview.app \
#     com.example.app "Apple Distribution: …" ~/Library/MobileDevice/Provisioning\ Profiles/<uuid>.mobileprovision \
#     laufey.ipa
set -euo pipefail

APP="$1"; BUNDLE_ID="$2"; IDENTITY="$3"; PROFILE="$4"; OUT="${5:-app.ipa}"
WORK="$(mktemp -d)"

# 1. Match the bundle id to the provisioning profile's App ID.
chmod u+w "$APP/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $BUNDLE_ID" "$APP/Info.plist"

# 2. Embed the provisioning profile.
cp "$PROFILE" "$APP/embedded.mobileprovision"

# 3. Use the profile's entitlements for signing.
security cms -D -i "$PROFILE" > "$WORK/profile.plist"
/usr/libexec/PlistBuddy -x -c "Print :Entitlements" "$WORK/profile.plist" > "$WORK/entitlements.plist"

# 4. Sign (deep, with entitlements).
codesign --force --sign "$IDENTITY" --entitlements "$WORK/entitlements.plist" \
  --timestamp=none "$APP"
codesign --verify --verbose "$APP"

# 5. Package Payload/<app> -> .ipa
rm -rf "$WORK/Payload"; mkdir -p "$WORK/Payload"
cp -R "$APP" "$WORK/Payload/"
(cd "$WORK" && zip -qry "$OLDPWD/$OUT" Payload)
rm -rf "$WORK"
echo "wrote $OUT"
