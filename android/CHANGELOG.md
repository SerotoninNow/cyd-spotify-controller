# CYD Now Playing: changes

Each version is a tag (`app-v…`) with a signed APK on the
[Releases page](https://github.com/SerotoninNow/cyd-spotify-controller/releases). All releases
are signed with the same key, so each installs over the one before.

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
