# Testing the app on a phone

Not yet tried on real hardware. With the remote firmware from this branch flashed and
`adb logcat -s CydLink MediaListener` running alongside (optional):

1. **Pairing still works**: forget CYD Remote, pair again; the five big buttons work as before.
2. **Install and grant**: install the APK, open it, turn on notification access (note whether
   Samsung shows "Restricted setting" and whether *Allow restricted settings* fixes it), allow
   nearby devices. CYD Remote is ticked under *Your CYD*.
3. **Connects**: the status reads "Connected to CYD Remote" within a few seconds, and the CYD
   switches to the now-playing layout ("Nothing playing" if nothing is).
4. **Song details**: play something in Spotify: title, artist, album, player name, cover and
   progress appear. Skip tracks a few times, quickly too: the cover always matches the song.
5. **Other players**: YouTube Music, a podcast app, a video in Chrome. Pausing one and playing
   another moves the screen to the one playing.
6. **Buttons**: previous, next, play/pause (the icon flips and stays right), volume (the % under
   the bar follows, also when using the phone's own buttons). Holding a volume button repeats.
7. **Seeking** in the player moves the CYD's progress bar within a second or so.
8. **Background**: close the app from recents, lock the phone for 10+ minutes, unlock and
   change songs: the CYD still updates. (If not, set Battery to Unrestricted and retry.)
9. **Reconnects**: power-cycle the CYD; it comes back with the song and cover without opening
   the app. Turn Bluetooth off and on on the phone: the same.
10. **Out of range**: walk away until it disconnects (the CYD returns to the big buttons), then
    come back.
11. **Headphones**: wired/USB-C or Bluetooth headphones keep playing; the CYD keeps showing
    the song.
12. **Odd text**: a song with accents (shown without them) and one in another script (`?`).
13. **Share**: *Share this app* → Quick Share to another phone; it installs from My Files there.
14. **Release APK**: uninstall the debug build, install the APK from the GitHub release (or the
    pull request's artifact), grant access again: everything above still works (that build is
    minified, so this is worth a quick run-through).
