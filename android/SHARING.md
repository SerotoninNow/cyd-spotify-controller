# Sharing CYD Now Playing with friends

The app isn't on the Play Store, but a friend can still install it and get every update
automatically — no cables, and you never have to send them a new file. The tool for that is
**Obtainium**, a free app that watches this project's releases and installs new versions.

Send a friend these steps.

## One-time setup (a couple of minutes)

1. **Install Obtainium.** On the phone, open
   <https://github.com/ImranR98/Obtainium/releases>, download the latest release APK, and open
   it. Allow "install unknown apps" if asked. (Or install Obtainium from the IzzyOnDroid
   F-Droid repo.)

2. **Add the app.** In Obtainium tap **Add App** and paste this URL:

   ```
   https://github.com/SerotoninNow/cyd-spotify-controller
   ```

3. In that app's settings, set **Filter Releases by Tag (regular expression)** to:

   ```
   ^app-v
   ```

   Leave the rest at their defaults and tap **Add**. Obtainium installs the latest release;
   allow "install unknown apps" for Obtainium if it asks.

4. **Grant access.** Open **CYD Now Playing** and give it the two permissions it requests:
   *notification access* (how it reads what's playing) and *nearby devices* (Bluetooth). Then
   pick your **CYD Remote** under *Your CYD*. Full details and troubleshooting are in the
   [README](../README.md).

## From then on

Turn on **Background updates** in Obtainium's settings. It checks for new releases on its own
and offers each one with a single tap — nothing for you to send around.

## If a future Android version blocks the install

Android is phasing in rules that can stop phones installing apps from unverified developers.
If a friend's phone ever refuses the install, the app can still go on over a USB cable with no
account, fee or sign-up — see *Installing and updating the app without the Play Store* in the
[README](../README.md).
