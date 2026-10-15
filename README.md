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

## Troubleshooting

- **Touches land off-target**: adjust the `TOUCH_*_MIN/MAX` values in `firmware/src/main.cpp`.
- **Colours inverted**: add `-DTFT_INVERSION_ON=1` to the build flags of your env.
- **Frames get lost**: set `LINK_BAUD` in `firmware/platformio.ini` to `921600`, reflash, and
  pass `--baud 921600`.
- **Seeing touches**: run with `CYD_DEBUG=1` to print each touch and the button it hit.
