# Spotify now playing on an ESP32-2432S028

The CYD ("Cheap Yellow Display") plugs into a USB port on your PC or Raspberry Pi and shows
what's playing: album art, title, artist and progress, with touch buttons for shuffle, previous,
play/pause, next, repeat (off, queue, song), and a heart that adds the song to your Liked Songs.

It works with Spotify and any other MPRIS player (Firefox, VLC...), preferring Spotify when
several are open. The heart only appears for Spotify songs, once you've signed in (step 3).

## 1. Flash the ESP32

PlatformIO lives in a virtual environment in this folder (many distros have no system `pip`):

```bash
python3 -m venv .venv && .venv/bin/pip install platformio
```

```bash
.venv/bin/pio run -d firmware -t upload
```

If your board has two USB ports (micro-USB and USB-C), add `-e cyd2usb` to that command.
If the upload can't connect, hold the BOOT button while it starts.

The firmware is the same as in [cyd-pi-display](https://github.com/SerotoninNow/cyd-pi-display),
so a board flashed for either works with both.

## 2. Run it

```bash
sudo apt install python3-serial python3-pil playerctl
```

On Arch or CachyOS: `sudo pacman -S python-pyserial python-pillow playerctl`.

```bash
python3 cyd_spotify.py
```

Your user needs access to the serial port: the `dialout` group on Debian and Raspberry Pi OS,
`uucp` on Arch (`sudo usermod -aG uucp $USER`, then log out and back in).

Options: `--port` (default `/dev/ttyUSB0`), `--player` (which player to prefer),
`--quality`, `--baud`.

## 3. Liking songs (optional)

The heart uses the Spotify Web API, so it needs a one-time setup:

1. At [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard), create an app
   (the app owner needs Spotify Premium). Tick "Web API" and add the redirect URI
   `http://127.0.0.1:8898/callback`.
2. Copy the app's Client ID and sign in:

   ```bash
   python3 cyd_spotify.py --spotify-login YOUR_CLIENT_ID
   ```

   This opens a browser to approve access and saves a token in `~/.config/cyd-display/spotify.json`.

Tap the heart to add the current song to Liked Songs; it fills green. Tap again to remove it.

## 4. Start it when you log in (optional)

On desktops using systemd (most Linux distros, Raspberry Pi OS), install the user service
from this folder:

```bash
mkdir -p ~/.config/systemd/user
```

```bash
sed "s|%h/cyd-spotify|$PWD|" cyd-spotify.service > ~/.config/systemd/user/cyd-spotify.service
```

```bash
systemctl --user daemon-reload && systemctl --user enable --now cyd-spotify
```

It starts with your desktop session, restarts if it crashes, and keeps retrying while the CYD is
unplugged. Check on it with `systemctl --user status cyd-spotify`, read its log with
`journalctl --user -u cyd-spotify -f`, and turn it off with `systemctl --user disable --now cyd-spotify`.
If you move this folder, run the `sed` command again.

## Standalone: no computer needed (experimental)

With different firmware, the CYD talks to Spotify over Wi-Fi by itself, so USB only powers it:
a phone charger, a Windows PC, anything. It shows and controls whatever your Spotify account is
playing, on any device (phone, PC, speaker). Differences from the version above:

- Playback controls need **Spotify Premium** (a Spotify rule for apps).
- Updates arrive every couple of seconds rather than every second.
- Song titles in non-Latin scripts show as `?`, and accents are dropped (the fonts are ASCII only).

**S1. Flash it** (stop the PC version first: `systemctl --user stop cyd-spotify`):

```bash
.venv/bin/pio run -d firmware -e standalone -t upload
```

Use `-e standalone2usb` for the two-port board. The screen then says it isn't set up yet.

**S2. Set it up** with the board still plugged into this computer. In your Spotify app on the
developer dashboard, the redirect URI `http://127.0.0.1:8898/callback` must be listed (step 3).

```bash
python3 cyd_spotify.py --setup-standalone
```

It asks for your Wi-Fi name, a username (only for networks like eduroam; press Enter otherwise)
and password (2.4 GHz networks only; the ESP32 has no 5 GHz radio),
opens Spotify in the browser to approve access, then sends both to the CYD, which keeps them in
its own flash and restarts. Nothing is saved on the computer. If you never ran `--spotify-login`,
add your Client ID after `--setup-standalone`.

For eduroam, the username is your full school address (`you@school.edu`). Username networks
use PEAP, and the network's certificate is not checked, so a fake network with the same name
could collect the login: if your school offers a separate device or Wi-Fi password, use that.
Guest networks with a sign-in web page don't work; the screen says so and shows the board's MAC
address, which some networks let you register instead.

Run the setup again to change Wi-Fi network. Anyone who has the board and a USB cable can read
the stored Wi-Fi password and Spotify sign-in back out of it, so treat it like a signed-in phone.

**Going back** to the computer version: flash `-e cyd` (or `-e cyd2usb`) again.

## Bluetooth: no Wi-Fi, no computer (experimental)

With this firmware your phone pairs with the CYD as a Bluetooth speaker. The CYD then shows
the song title, artist, album and progress, plus the album cover when the phone sends one. Touch
buttons cover previous, play/pause, next and volume down/up. It works with any app that plays
on the phone (Spotify, YouTube Music, podcasts...), with no Wi-Fi, no Spotify account and no
computer. USB only powers it.

Phones only send song details to the Bluetooth device they are playing to, so the phone has to
treat the CYD as its speaker. Where the sound actually goes is up to you:

- **The speaker socket** (`-e bluetooth`): plug a small speaker (8 ohm, up to about 1 W,
  JST 1.25 mm 2-pin) into the socket marked SPEAK. It is 8-bit mono, so treat it as a preview
  speaker, not a hi-fi.
- **An I2S DAC board** (`-e bluetooth-i2s`): proper 16-bit stereo for headphones, an amplifier
  or powered speakers, from a PCM5102 board (a few dollars). Wire it as below.
- **Another Bluetooth speaker, with the CYD silent**: on phones that play to two Bluetooth
  devices at once (Samsung *Dual audio* in the media output panel), turn the CYD's sound off by
  holding a finger on the cover or title for a second. It still shows the song and its buttons
  still work, and the other speaker plays. Hold again to turn the sound back on; it's
  remembered across restarts. While it's off, the volume buttons ask the phone to change its
  volume instead, which some phones ignore (the phone's own buttons always work).

Without dual audio, a phone plays to one Bluetooth device at a time, and plugging headphones into
the phone moves the sound away from the CYD, which may then stop getting song changes.

I2S wiring (CYD to PCM5102 board): the CN1 connector gives 3.3V to VIN, GND to GND,
GPIO27 to BCK and GPIO22 to LCK (word select). The P1 serial connector gives TX to DIN.
Tie the board's SCK pin to GND. The common purple boards also have four solder jumpers on the
back; if they're blank (no sound at all), bridge FLT, DEMP and FMT to L and XSMT to H. Other pins: add `-DI2S_BCK=`, `-DI2S_WS=` or `-DI2S_DOUT=` to the env's
build flags. Using TX means this build's serial log stays silent; uploading still works.

**B1. Flash it:**

```bash
.venv/bin/pio run -d firmware -e bluetooth -t upload
```

Use `-e bluetooth2usb` for the two-port board (`-e bluetooth-i2s`, `-e bluetooth-i2s-2usb` for
I2S). The first build downloads a newer ESP32
toolchain (Arduino core 3), which takes a few minutes. If later builds then fail with
`No module named 'intelhex'`, run `.venv/bin/pip install intelhex`.

**B2. Pair:** the screen says "Pair your phone with CYD Player". Pick it in your phone's
Bluetooth settings. After that it reconnects whenever the phone is nearby, and on power-up the
CYD calls the last phone itself.

- **Volume**: the CYD's buttons and the phone's volume buttons move the same setting (Bluetooth
  absolute volume), shown under the progress bar ("Sound off" when muted). Hold a volume button
  to keep changing it.
- **Album covers** need AVRCP 1.6 cover art. iPhones send them. On Android, if you only see a
  music-note tile, turn on Developer options, set *Bluetooth AVRCP version* to **1.6**, then
  turn Bluetooth off and on. Some apps send no cover at all; the title still shows.
- **Unpairing**: hidden from new phones while one is connected. To forget every paired phone,
  open `.venv/bin/pio device monitor -d firmware -e bluetooth` and type `FORGET`.
- Titles in non-Latin scripts show as `?`, as in the Wi-Fi version.

**Going back**: flash `-e cyd`, `-e standalone` (or their `2usb` versions) again.

## Troubleshooting

- **Touches land off-target**: adjust the `TOUCH_*_MIN/MAX` values in `firmware/src/main.cpp`.
- **Colours inverted**: add `-DTFT_INVERSION_ON=1` to the build flags of your env.
- **Frames get lost**: set `LINK_BAUD` in `firmware/platformio.ini` to `921600`, reflash, and
  pass `--baud 921600`.
- **Seeing touches**: run with `CYD_DEBUG=1` to print each touch and the button it hit.
- **Bluetooth version misbehaving**: `.venv/bin/pio device monitor -d firmware -e bluetooth`
  shows its log (connections, sample rate, whether the phone offers covers).
- **Standalone version misbehaving**: `.venv/bin/pio device monitor -d firmware -e standalone` shows
  its log (Wi-Fi, Spotify errors). Close it before running the setup.
