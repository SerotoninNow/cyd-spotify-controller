# CYD Now Playing: changes

Each version is a tag (`app-v…`) with a signed APK on the
[Releases page](https://github.com/SerotoninNow/cyd-spotify-controller/releases). All releases
are signed with the same key, so each installs over the one before.

## Unreleased

- New: updates the CYD's firmware over Bluetooth. The app carries the remote firmware from
  the same commit; a *CYD firmware* section shows what the CYD runs and offers *Update the
  CYD* when they differ (about two minutes, with progress on both screens). The CYD checks
  the image's SHA-256 and keeps its firmware if anything fails. Needs the remote firmware
  from the same commit, flashed over USB once: older builds have no room for an update.

## 1.0.3 (2026-10-20)

- Faster reconnects: after the CYD restarts, the app now tries again after half a second
  instead of 5 seconds (slowing down to 5 seconds only if it keeps failing). This also covers
  the first write after power-up, which the phone sometimes refuses with status 133 while it
  re-encrypts the link. Song details are back about 6 seconds after a restart instead of 11;
  most of what's left is the phone noticing the CYD went away.

## 1.0.2 (2026-10-20)

- Fixed: with the CYD paired after the app had started, the status stayed at "No CYD chosen"
  and nothing was sent, even though CYD Remote showed as picked (tapping it did nothing, as it
  was already ticked). The app now looks for the CYD again whenever it's opened, and every
  15 seconds in the background, until it has one.
- First run on real hardware (Galaxy S22, Android 16): song details, covers and updates arrive. If the
  CYD shows the first song and then never changes, check the app is 1.0.1 or later: 1.0
  crashes as soon as it sends a cover.

## 1.0.1 (2026-10-19)

- Fixed: the app crashed on every start once it began sending a cover to the CYD, so it
  couldn't be opened (Samsung offered to clear its cache, which didn't help). With the usual
  MTU of 517 it sent covers in 514-byte writes, but Android allows at most 512 bytes per write
  and throws otherwise; the throw came from the background listener, which shares the app's
  process. Writes are now capped at 512 bytes, and a write Android refuses is skipped instead
  of crashing. Commit 82d9a55.

## 1.0 (2026-10-19)

First release. **Has the crash above: use 1.0.1.** Kept for the record.

- Reads the phone's active media session through notification access (any player, no
  account) and sends the title, artist, album, player, play state, position, volume and a
  128×128 JPEG cover to the CYD remote over Bluetooth LE, alongside its media buttons.
- "Share this app" sends the installed APK to another phone.
- The protocol is described at the top of `firmware/src/remote.cpp`; it needs the remote
  firmware from the same commit or later.
