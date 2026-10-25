#!/bin/sh
# Install the CYD Now Playing app onto a phone over ADB. ADB is the developer channel, which
# stays exempt from Android's Play-Store / sideloading developer-verification rules, so this
# needs no Google account, no fee and no app registration. It works over USB, or wirelessly
# once the phone is paired (Developer options > Wireless debugging, then `adb pair <ip:port>`
# followed by `adb connect <ip:port>`).
#
# Usage:
#   android/install-app.sh                  # download the latest release and install it
#   android/install-app.sh path/to/app.apk  # install a local APK instead
#   REPO=owner/name android/install-app.sh  # force which GitHub repo to take the release from
#
# Needs: adb (Android platform-tools). Downloading a release also needs gh (GitHub CLI).
# With more than one device on ADB, set ANDROID_SERIAL=<serial> (see `adb devices`).
set -eu

here=$(CDPATH= cd "$(dirname "$0")" && pwd)

if ! adb get-state >/dev/null 2>&1; then
  echo "No phone on ADB. Connect it over USB with USB debugging on, or over wireless debugging" >&2
  echo "(adb pair <ip:port>, then adb connect <ip:port>); 'adb devices' should list it." >&2
  exit 1
fi

apk=${1:-}
tmp=
if [ -z "$apk" ]; then
  command -v gh >/dev/null 2>&1 || {
    echo "gh (GitHub CLI) is needed to download a release, or pass a local APK path." >&2
    exit 1
  }
  if [ -z "${REPO:-}" ]; then
    # zeke checkout -> the private repo (its 'private' remote); otherwise the public 'origin'.
    branch=$(git -C "$here" rev-parse --abbrev-ref HEAD 2>/dev/null || echo main)
    case "$branch" in
      zeke*) remote=private ;;
      *) remote=origin ;;
    esac
    url=$(git -C "$here" remote get-url "$remote")
    REPO=$(printf '%s' "$url" | sed -E 's#^(git@github.com:|https://github.com/)##; s#\.git$##')
  fi
  tmp=$(mktemp -d)
  echo "Downloading the latest release from $REPO ..."
  gh release download --repo "$REPO" --pattern 'CYD-Now-Playing-*.apk' --dir "$tmp"
  apk=$(ls "$tmp"/CYD-Now-Playing-*.apk | head -1)
fi

echo "Installing $(basename "$apk") ..."
adb install -r "$apk"
if [ -n "$tmp" ]; then rm -rf "$tmp"; fi
echo "Done. Releases share one signing key, so each installs straight over the last."
