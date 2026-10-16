#!/usr/bin/env python3
"""Spotify "now playing" on an ESP32-2432S028 (CYD) connected over USB.

    python3 cyd_spotify.py                              # run it (CYD plugged into a USB port)
    python3 cyd_spotify.py --spotify-login CLIENT_ID    # one-time sign-in so the heart can like songs

Shows album art, title, artist and progress, with touch buttons for shuffle, previous, play/pause,
next, repeat and Liked Songs. Works with any MPRIS player through playerctl, preferring Spotify.
See ../firmware/src/main.cpp for the link protocol.
"""
import io
import os
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

import serial
from PIL import Image, ImageChops, ImageDraw, ImageFont

W, H = 320, 240
TILE = 16  # partial updates are aligned to JPEG blocks so their edges don't show
MAX_FRAME = 60000
ACK_TIMEOUT = 1.5


def load_font(size, bold=False):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    for folder in ("/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/TTF", "/usr/share/fonts/dejavu"):
        try:
            return ImageFont.truetype(os.path.join(folder, name), size)
        except OSError:
            pass
    return ImageFont.load_default(size)


SPOTIFY_CONFIG = os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")),
                              "cyd-display", "spotify.json")
SPOTIFY_REDIRECT = "http://127.0.0.1:8898/callback"


def _http_json(url, data=None, method=None, headers=None):
    import json
    import urllib.request

    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    with urllib.request.urlopen(req, timeout=10) as r:
        body = r.read()
    return json.loads(body) if body else None


def spotify_authorize(client_id, scope, purpose):
    """Spotify sign-in in the browser (OAuth with PKCE, so no client secret is needed).
    Returns a refresh token."""
    import base64
    import hashlib
    import http.server
    import secrets
    import urllib.parse
    import webbrowser

    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(16)
    url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(dict(
        client_id=client_id, response_type="code", redirect_uri=SPOTIFY_REDIRECT, state=state,
        scope=scope, code_challenge_method="S256", code_challenge=challenge))
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            if q.get("state", [""])[0] == state:
                result.update({k: v[0] for k, v in q.items()})
            ok = "code" in result
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(b"Signed in; you can close this tab." if ok else b"Sign-in failed; see the terminal.")

        def log_message(self, *_):
            pass

    host, port = urllib.parse.urlparse(SPOTIFY_REDIRECT).netloc.split(":")
    with http.server.HTTPServer((host, int(port)), Handler) as server:
        print(f"Open this link to let the CYD {purpose}:\n\n  {url}\n")
        webbrowser.open(url)
        while not result:
            server.handle_request()
    if "code" not in result:
        raise SystemExit(f"Spotify sign-in failed: {result.get('error', 'no code returned')}")
    tok = _http_json("https://accounts.spotify.com/api/token", urllib.parse.urlencode(dict(
        grant_type="authorization_code", code=result["code"], redirect_uri=SPOTIFY_REDIRECT,
        client_id=client_id, code_verifier=verifier)).encode())
    return tok["refresh_token"]


def spotify_login(client_id):
    """One-time sign-in for liking songs from this computer. Saves the refresh token."""
    import json

    token = spotify_authorize(client_id, "user-library-read user-library-modify",
                              "like songs on your Spotify account")
    os.makedirs(os.path.dirname(SPOTIFY_CONFIG), exist_ok=True)
    fd = os.open(SPOTIFY_CONFIG, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(dict(client_id=client_id, refresh_token=token), f)
    print(f"Signed in. Saved to {SPOTIFY_CONFIG}")


def setup_standalone(port_name, client_id):
    """Sends Wi-Fi details and a Spotify sign-in to a CYD running the standalone firmware,
    which keeps them in its own flash. Nothing is saved on this computer."""
    import getpass
    import json

    if not client_id:
        try:
            with open(SPOTIFY_CONFIG) as f:
                client_id = json.load(f)["client_id"]
        except (OSError, ValueError, KeyError):
            raise SystemExit("Give your Spotify app's Client ID: --setup-standalone CLIENT_ID (see README)")

    try:
        port = serial.Serial(port_name, 115200, timeout=0.5)
    except serial.SerialException as err:
        raise SystemExit(f"{port_name}: {err}\nIf the PC version is running, stop it first "
                         "(systemctl --user stop cyd-spotify).")
    with port:
        # Restart the board (EN low through RTS, with IO0 left high so it boots normally).
        port.dtr, port.rts = False, True
        time.sleep(0.1)
        port.rts = False

        def command(line, timeout=5):
            port.write((line + "\n").encode())
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                reply = port.readline().decode(errors="replace").strip()
                if reply == "OK":
                    return
                if reply.startswith("ERR"):
                    raise SystemExit(f"The CYD refused '{line.split('=')[0]}': {reply}")
            raise SystemExit("The CYD didn't answer. Is the standalone firmware on it? (README, step S1)")

        end = time.monotonic() + 6
        while time.monotonic() < end and b"CYD-STANDALONE READY" not in port.readline():
            pass
        command("INFO")  # also confirms it's listening if the ready line was missed
        print("Found the CYD.\n")

        ssid = input("Wi-Fi network name (2.4 GHz): ").strip()
        user = input("Username, for networks like eduroam (press Enter if there is none): ").strip()
        password = getpass.getpass("Wi-Fi password (not shown): ")
        if not ssid or "\n" in ssid + user + password:
            raise SystemExit("That network name, username or password can't be used.")
        token = spotify_authorize(
            client_id, "user-read-playback-state user-modify-playback-state user-library-read user-library-modify",
            "show and control your Spotify playback")
        for key, value in (("wifi_ssid", ssid), ("wifi_user", user), ("wifi_pass", password), ("client_id", client_id),
                           ("refresh_token", token)):
            command(f"SET {key}={value}")
        command("REBOOT")
    print(f"\nDone. The CYD will join {ssid} and show what's playing on your Spotify account.\n"
          "From now on it only needs USB power: a phone charger or any computer will do.")


class SpotifyLikes:
    """Liked Songs through the Spotify Web API, using the token saved by --spotify-login."""

    API = "https://api.spotify.com/v1/me/library"

    def __init__(self):
        import json

        with open(SPOTIFY_CONFIG) as f:
            self.conf = json.load(f)
        self.token, self.expires = None, 0.0
        self.lock = threading.Lock()

    def _auth(self):
        import json
        import urllib.parse

        with self.lock:
            if time.monotonic() > self.expires - 60:
                tok = _http_json("https://accounts.spotify.com/api/token", urllib.parse.urlencode(dict(
                    grant_type="refresh_token", refresh_token=self.conf["refresh_token"],
                    client_id=self.conf["client_id"])).encode())
                self.token, self.expires = tok["access_token"], time.monotonic() + tok.get("expires_in", 3600)
                if tok.get("refresh_token", self.conf["refresh_token"]) != self.conf["refresh_token"]:
                    self.conf["refresh_token"] = tok["refresh_token"]  # Spotify may rotate it
                    with open(SPOTIFY_CONFIG, "w") as f:
                        json.dump(self.conf, f)
            return {"Authorization": f"Bearer {self.token}"}

    def _call(self, method, path, uri):
        import urllib.parse

        url = f"{self.API}{path}?uris={urllib.parse.quote(uri)}"
        return _http_json(url, data=b"" if method != "GET" else None, method=method, headers=self._auth())

    def is_liked(self, uri):
        return bool(self._call("GET", "/contains", uri)[0])

    def set_liked(self, uri, liked):
        self._call("PUT" if liked else "DELETE", "", uri)


class MusicCapture:
    """Now playing on any MPRIS player (Spotify first), with touch buttons. Uses playerctl."""

    BG, FG, DIM, TRACK, ACCENT = (12, 14, 20), (235, 238, 245), (140, 146, 160), (40, 44, 56), (30, 215, 96)
    FIELDS = ("status", "playerName", "xesam:title", "xesam:artist", "xesam:album", "mpris:artUrl",
              "mpris:length", "position", "mpris:trackid", "xesam:url", "shuffle", "loop")
    ART = 128
    BUTTONS = ("shuffle", "prev", "play", "next", "repeat", "like")
    REPEAT_NEXT = {"None": "Playlist", "Playlist": "Track", "Track": "None"}  # off, queue, song, as in Spotify
    BTN_Y0, BTN_Y1 = 186, 234

    def __init__(self, player="spotify"):
        if not shutil.which("playerctl"):
            raise SystemExit("playerctl is needed. Install it with "
                             "'sudo apt install playerctl' or 'sudo pacman -S playerctl'.")
        self.players = f"{player},%any" if player else "%any"
        self.title_font, self.font, self.small = load_font(17, True), load_font(14), load_font(11)
        self.info, self.polled, self.pos_at = None, 0.0, 0.0
        self.frame, self.frame_key = None, None  # last drawing, and the state it was drawn from
        self.art_cache, self.art_loading = {}, set()
        self.pressed = None
        self.box = (0, 0, W, H)
        self.liked = {}  # Spotify URI -> True/False; missing while unknown
        try:
            self.likes = SpotifyLikes()
        except (OSError, ValueError, KeyError):
            self.likes = None
            print("music: liking songs is off; set it up with --spotify-login CLIENT_ID (see README)")

    # --- player state ---

    def _poll(self):
        fmt = "\t".join("{{%s}}" % f for f in self.FIELDS)
        out = subprocess.run(["playerctl", "-p", self.players, "metadata", "--format", fmt],
                             capture_output=True, text=True)
        if out.returncode != 0 or not out.stdout.strip():
            return None
        vals = out.stdout.rstrip("\n").split("\t")
        if len(vals) != len(self.FIELDS):
            return None
        info = dict(zip(self.FIELDS, vals))
        info["length"] = int(info["mpris:length"] or 0) / 1e6
        info["position"] = int(info["position"] or 0) / 1e6
        info["uri"] = self._spotify_uri(info)
        return info

    @staticmethod
    def _spotify_uri(info):
        tid, url = info["mpris:trackid"], info["xesam:url"]
        if tid.startswith("/com/spotify/track/"):
            return "spotify:track:" + tid.rsplit("/", 1)[1]
        if "open.spotify.com/track/" in url:
            return "spotify:track:" + url.split("/track/", 1)[1].split("?")[0]
        return None

    def _position(self):
        i = self.info
        pos = i["position"] + (time.monotonic() - self.pos_at if i["status"] == "Playing" else 0)
        return min(pos, i["length"]) if i["length"] else pos

    def _background(self, fn, *args, **kwargs):
        threading.Thread(target=fn, args=args, kwargs=kwargs, daemon=True).start()

    def _load_art(self, url):
        try:
            if url.startswith("file://"):
                import urllib.parse

                img = Image.open(urllib.parse.unquote(url[7:]))
            else:
                import urllib.request

                with urllib.request.urlopen(url, timeout=10) as r:
                    img = Image.open(io.BytesIO(r.read()))
            self.art_cache[url] = img.convert("RGB").resize((self.ART, self.ART), Image.LANCZOS)
        except Exception:
            self.art_cache[url] = None
        finally:
            self.art_loading.discard(url)

    def _load_liked(self, uri):
        try:
            self.liked[uri] = self.likes.is_liked(uri)
        except Exception as err:
            print(f"music: can't read Liked Songs: {err}", file=sys.stderr)

    def _toggle_like(self, uri):
        want = not self.liked.get(uri, False)
        self.liked[uri] = want  # show it straight away; put back if the request fails
        try:
            self.likes.set_liked(uri, want)
            print(f"music: {'added to' if want else 'removed from'} Liked Songs")
        except Exception as err:
            self.liked[uri] = not want
            print(f"music: can't change Liked Songs: {err}", file=sys.stderr)

    # --- touch ---

    def _button_at(self, x, y):
        if not self.BTN_Y0 - 6 <= y <= self.BTN_Y1 + 6:
            return None
        return self.BUTTONS[min(max(x * len(self.BUTTONS) // W, 0), len(self.BUTTONS) - 1)]

    def event(self, x, y, pressed):
        if os.environ.get("CYD_DEBUG"):
            print(f"touch {x} {y} {int(pressed)} -> {self._button_at(x, y)}")
        if pressed:
            if self.pressed is None:
                self.pressed = self._button_at(x, y)
            return
        button, self.pressed = self.pressed, None
        info = self.info
        if not button or not info:
            return
        if button == "like":
            if self.likes and info["uri"]:
                self._background(self._toggle_like, info["uri"])
            return
        if button == "shuffle":
            on = info["shuffle"] != "true"
            info["shuffle"] = "true" if on else "false"  # show it straight away; the next poll confirms
            cmd = ["shuffle", "On" if on else "Off"]
        elif button == "repeat":
            info["loop"] = self.REPEAT_NEXT.get(info["loop"], "None")
            cmd = ["loop", info["loop"]]
        else:
            cmd = [{"prev": "previous", "play": "play-pause", "next": "next"}[button]]
        self._background(subprocess.run, ["playerctl", "-p", info["playerName"], *cmd], capture_output=True)
        self.polled = 0  # pick up the change on the next frame

    # --- drawing ---

    def _fit_text(self, d, text, font, width):
        if d.textlength(text, font=font) <= width:
            return text
        while text and d.textlength(text + "…", font=font) > width:
            text = text[:-1]
        return text.rstrip() + "…"

    def _wrap(self, d, text, font, width, lines):
        words, out = text.split(), []
        while words and len(out) < lines:
            line = words.pop(0)
            while words and d.textlength(line + " " + words[0], font=font) <= width:
                line += " " + words.pop(0)
            out.append(line)
        if words:
            out[-1] = self._fit_text(d, out[-1] + " " + " ".join(words), font, width)
        return [self._fit_text(d, line, font, width) for line in out]

    @staticmethod
    def _clock(s):
        s = int(s)
        return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}" if s >= 3600 else f"{s // 60}:{s % 60:02d}"

    def _icon(self, d, name, cx, cy, color, info, liked):
        playing = info["status"] == "Playing"
        if name == "shuffle":
            on = info["shuffle"] == "true"
            c = self.ACCENT if on else color
            for y0, y1 in ((cy - 7, cy + 7), (cy + 7, cy - 7)):
                d.line((cx - 11, y0, cx + 6, y1), fill=c, width=2)
                d.polygon(((cx + 12, y1), (cx + 5, y1 - 5), (cx + 5, y1 + 5)), fill=c)
            if on:
                d.ellipse((cx - 2, cy + 14, cx + 2, cy + 18), fill=c)
        elif name == "repeat":
            mode = info["loop"]
            c = self.ACCENT if mode in ("Playlist", "Track") else color
            d.rounded_rectangle((cx - 11, cy - 7, cx + 11, cy + 7), 5, outline=c, width=2)
            d.rectangle((cx - 8, cy - 9, cx - 5, cy - 5), fill=self.BG)  # gap behind the arrowhead
            d.polygon(((cx - 2, cy - 6), (cx - 7, cy - 11), (cx - 7, cy - 1)), fill=c)
            if mode == "Track":
                d.line((cx + 1, cy - 3, cx + 1, cy + 3), fill=c, width=2)  # a "1"; text is too blurry at this size
                d.line((cx - 1, cy - 1, cx + 1, cy - 3), fill=c, width=2)
            if mode in ("Playlist", "Track"):
                d.ellipse((cx - 2, cy + 14, cx + 2, cy + 18), fill=c)
        elif name == "play" and playing:
            d.rectangle((cx - 9, cy - 11, cx - 3, cy + 11), fill=color)
            d.rectangle((cx + 3, cy - 11, cx + 9, cy + 11), fill=color)
        elif name == "play":
            d.polygon(((cx - 8, cy - 12), (cx - 8, cy + 12), (cx + 12, cy)), fill=color)
        elif name in ("prev", "next"):
            s = -1 if name == "prev" else 1
            for off in (-8, 2):
                tip = cx + s * (off + 10)
                d.polygon(((cx + s * off, cy - 9), (cx + s * off, cy + 9), (tip, cy)), fill=color)
            d.rectangle((cx + s * 12 - 1, cy - 9, cx + s * 12 + 1, cy + 9), fill=color)
        elif name == "like":
            r = 6
            pts = ((cx - 2 * r, cy - 2), (cx, cy + 2 * r), (cx + 2 * r, cy - 2))
            if liked:
                d.ellipse((cx - 2 * r, cy - r - 4, cx, cy + r - 4), fill=self.ACCENT)
                d.ellipse((cx, cy - r - 4, cx + 2 * r, cy + r - 4), fill=self.ACCENT)
                d.polygon(pts, fill=self.ACCENT)
            else:
                d.arc((cx - 2 * r, cy - r - 4, cx, cy + r - 4), 135, 360, fill=color, width=2)
                d.arc((cx, cy - r - 4, cx + 2 * r, cy + r - 4), 180, 45, fill=color, width=2)
                d.line((pts[0][0] + 2, pts[0][1] + 2, cx, cy + 2 * r), fill=color, width=2)
                d.line((cx, cy + 2 * r, pts[2][0] - 2, pts[2][1] + 2), fill=color, width=2)

    def grab(self):
        now = time.monotonic()
        if now - self.polled > 1.0:
            self.polled = now
            prev, self.info = self.info, self._poll()
            self.pos_at = now
            if self.info and self.likes and self.info["uri"] and self.info["uri"] not in self.liked and (
                    not prev or prev["uri"] != self.info["uri"]):
                self._background(self._load_liked, self.info["uri"])

        # Redraw only when something visible changed: the panel mostly sits still, and drawing
        # plus comparing frames 30 times a second costs far more than this check.
        info = self.info
        key = None
        if info:
            pos = self._position()
            bar = int((W - 24) * min(max(pos / info["length"], 0.0), 1.0)) if info["length"] else 0
            key = (tuple(info[f] for f in self.FIELDS if f != "position"), int(pos), bar, self.pressed,
                   self.liked.get(info["uri"]), self.art_cache.get(info["mpris:artUrl"]) is not None)
        if self.frame is not None and key == self.frame_key:
            return self.frame
        self.frame_key = key
        self.frame = img = Image.new("RGB", (W, H), self.BG)
        d = ImageDraw.Draw(img)
        if not info:
            d.text((W // 2, H // 2), "Nothing playing", font=self.title_font, fill=self.DIM, anchor="mm")
            return img

        art_url = info["mpris:artUrl"]
        art = self.art_cache.get(art_url)
        if art is None and art_url and art_url not in self.art_cache and art_url not in self.art_loading:
            self.art_loading.add(art_url)
            self._background(self._load_art, art_url)
        ax, ay = 12, 12
        if art:
            img.paste(art, (ax, ay))
        else:
            d.rounded_rectangle((ax, ay, ax + self.ART - 1, ay + self.ART - 1), 6, fill=self.TRACK)

        tx, tw = ax + self.ART + 12, W - (ax + self.ART + 12) - 12
        y = 14
        for line in self._wrap(d, info["xesam:title"] or "Unknown", self.title_font, tw, 3):
            d.text((tx, y), line, font=self.title_font, fill=self.FG)
            y += 22
        y += 4
        for line in self._wrap(d, info["xesam:artist"], self.font, tw, 2):
            d.text((tx, y), line, font=self.font, fill=self.FG)
            y += 18
        if info["xesam:album"] and y < 118:
            d.text((tx, y + 2), self._fit_text(d, info["xesam:album"], self.small, tw), font=self.small, fill=self.DIM)
        d.text((tx, ay + self.ART - 12), info["playerName"].capitalize(), font=self.small, fill=self.DIM)

        by = 156
        d.rounded_rectangle((12, by, W - 12, by + 5), 2, fill=self.TRACK)
        if info["length"]:
            pos = self._position()
            frac = min(max(pos / info["length"], 0.0), 1.0)
            if frac > 0:
                d.rounded_rectangle((12, by, 12 + max(5, int((W - 24) * frac)), by + 5), 2, fill=self.ACCENT)
            d.text((12, by + 9), self._clock(pos), font=self.small, fill=self.DIM)
            d.text((W - 12, by + 9), self._clock(info["length"]), font=self.small, fill=self.DIM, anchor="ra")

        bw = W // len(self.BUTTONS)
        can_like = bool(self.likes and info["uri"])
        for i, name in enumerate(self.BUTTONS):
            if name == "like" and not can_like or name == "shuffle" and info["shuffle"] not in ("true", "false") \
                    or name == "repeat" and info["loop"] not in self.REPEAT_NEXT:
                continue  # this player doesn't support it
            x0 = i * bw
            if self.pressed == name:
                d.rounded_rectangle((x0 + 6, self.BTN_Y0, x0 + bw - 6, self.BTN_Y1), 10, fill=self.TRACK)
            self._icon(d, name, x0 + bw // 2, (self.BTN_Y0 + self.BTN_Y1) // 2, self.FG,
                       info, self.liked.get(info["uri"], False))
        return img


def encode(frame, quality):
    while True:
        buf = io.BytesIO()
        frame.save(buf, "JPEG", quality=quality)  # baseline; the ESP32 can't decode progressive
        if buf.tell() <= MAX_FRAME or quality <= 20:
            return buf.getvalue()
        quality -= 15


def reader(port, ack, touch):
    """Background thread: parses lines coming back from the ESP32."""
    try:
        while True:
            line = port.readline().strip()
            if line == b"K":
                ack.set()
            elif line.startswith(b"T ") and touch:
                try:
                    x, y, p = (int(v) for v in line.split()[1:4])
                except ValueError:
                    continue
                try:
                    touch.event(x, y, bool(p))
                except Exception as err:  # a failed touch must not stop the link
                    print(f"touch: {err!r}", file=sys.stderr)
    except (serial.SerialException, OSError, TypeError):
        ack.set()


def _with_length(data):
    return struct.pack("<I", len(data)) + data


def changed_area(frame, shown):
    """The block-aligned box that differs from what's on the panel, or None."""
    box = ImageChops.difference(frame, shown).getbbox()
    if not box:
        return None
    x0, y0 = box[0] // TILE * TILE, box[1] // TILE * TILE
    x1, y1 = min(W, -(-box[2] // TILE) * TILE), min(H, -(-box[3] // TILE) * TILE)
    return x0, y0, x1, y1


def stream(port, screen, args):
    ack = threading.Event()
    threading.Thread(target=reader, args=(port, ack, screen), daemon=True).start()
    period = 1.0 / args.fps
    shown = None  # the frame currently on the panel; None forces a full redraw
    while True:
        start = time.monotonic()
        frame = screen.grab()
        area = None if frame is shown else (0, 0, W, H) if shown is None else changed_area(frame, shown)
        if area:
            x0, y0, x1, y1 = area
            if (x1 - x0) * (y1 - y0) > W * H * 0.6:
                packet = b"CYDF" + _with_length(encode(frame, args.quality))
            else:
                data = encode(frame.crop(area), args.quality)
                packet = b"CYDR" + struct.pack("<HH", x0, y0) + _with_length(data)
            ack.clear()
            port.write(packet)
            # A lost frame leaves the panel in an unknown state, so the next one is a full redraw.
            shown = frame if ack.wait(ACK_TIMEOUT) else None
        delay = period - (time.monotonic() - start)
        if delay > 0:
            time.sleep(delay)


def main():
    import argparse

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=2000000, help="must match LINK_BAUD in platformio.ini")
    ap.add_argument("--player", default="spotify", help="MPRIS player to prefer (others are used when it is closed)")
    ap.add_argument("--fps", type=float, default=30, help="upper limit; the link usually caps it lower")
    ap.add_argument("--quality", type=int, default=70, help="JPEG quality 1-95")
    ap.add_argument("--spotify-login", metavar="CLIENT_ID",
                    help="sign in to Spotify once so the heart button can like songs, then exit")
    ap.add_argument("--setup-standalone", metavar="CLIENT_ID", nargs="?", const="",
                    help="set up a CYD running the standalone firmware (Wi-Fi and Spotify), then exit; "
                         "the Client ID defaults to the one from --spotify-login")
    args = ap.parse_args()
    if args.spotify_login:
        spotify_login(args.spotify_login)
        return
    if args.setup_standalone is not None:
        setup_standalone(args.port, args.setup_standalone)
        return

    screen = MusicCapture(args.player)
    while True:
        try:
            with serial.Serial(args.port, args.baud, timeout=1, write_timeout=5) as port:
                print(f"connected to {args.port}")
                stream(port, screen, args)
        except (serial.SerialException, OSError) as err:
            print(f"{args.port}: {err}; retrying", file=sys.stderr)
            time.sleep(2)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        main()
    except KeyboardInterrupt:
        pass
