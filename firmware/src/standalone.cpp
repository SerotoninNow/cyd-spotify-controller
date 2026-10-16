// ESP32-2432S028 (CYD) as a standalone Spotify remote. It talks to the Spotify Web API over
// Wi-Fi, so USB only powers it. Set it up once from a PC: python3 cyd_spotify.py --setup-standalone
//
// Setup protocol (USB serial, SETUP_BAUD 8N1, one command per line):
//   PC  -> ESP   "SET key=value"   store a setting: wifi_ssid, wifi_pass, client_id, refresh_token
//                "INFO"            which settings are stored (values are never printed)
//                "CLEAR"           forget every setting
//                "REBOOT"
//   ESP -> PC    "CYD-STANDALONE READY" after boot, then "OK" or "ERR ..." after each command.
//                Any other line is a log message.
//
// Two tasks: the network task (core 0) polls Spotify and sends commands, the Arduino loop
// (core 1) draws and reads the touchscreen, so a slow request never makes touch lag.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#include "root_cas.h"

#ifndef SETUP_BAUD
#define SETUP_BAUD 115200
#endif

// The touch controller sits on its own SPI bus on this board.
#define TOUCH_IRQ 36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK 25
#define TOUCH_CS 33

// Raw XPT2046 readings at the screen edges. Adjust if touches land off-target.
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

static const int W = 320, H = 240;
static const int ART = 128, ART_X = 12, ART_Y = 12;
static const int TEXT_X = ART_X + ART + 12, TEXT_W = W - TEXT_X - 12;
static const int BAR_Y = 156;
static const int BTN_Y0 = 186, BTN_Y1 = 234;
static const size_t ART_MAX = 65536;  // a 300x300 cover is usually 20-50 KB

enum Button : uint8_t { B_SHUFFLE, B_PREV, B_PLAY, B_NEXT, B_REPEAT, B_LIKE, N_BUTTONS, B_NONE = 255 };

static TFT_eSPI tft;
static JPEGDEC jpeg;
static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
static Preferences prefs;
static uint16_t BG, FG, DIM, TRACK, ACCENT;

// --- state shared between the tasks (guarded by `lock`) ---

struct Player {
  bool valid = false;  // something is loaded on an active device
  bool playing = false, shuffle = false;
  uint8_t repeat = 0;  // 0 off, 1 queue (Spotify calls it "context"), 2 song ("track")
  int8_t liked = -1;   // -1 while unknown
  String uri, title, artist, album, device;
  uint32_t durationMs = 0, progressMs = 0, at = 0;  // progressMs as read at millis() == at
};

static SemaphoreHandle_t lock;
static Player player;
static String status = "Starting...";  // shown full screen while nothing is playing
static String notice;                  // shown briefly in place of the device name
static uint32_t noticeUntil = 0;
static uint32_t version = 1;           // bumped whenever the screen should be redrawn

static uint16_t artPixels[ART * ART];  // written by the network task, drawn by the loop
static volatile bool artValid = false;
static volatile uint32_t artVersion = 0;

// A tap, from the loop to the network task. `value` is the new setting the screen already shows
// (playing, shuffle, repeat mode, liked), so a status check finishing in between can't undo it.
struct Action {
  uint8_t button, value;
  String uri;
};
static QueueHandle_t actions;

// Settings, read once at boot.
static String wifiSsid, wifiPass, clientId, refreshToken;

static void setStatus(const String &s) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (status != s) {
    status = s;
    version++;
  }
  xSemaphoreGive(lock);
  Serial.println(s);
}

static void setNotice(const String &s) {
  xSemaphoreTake(lock, portMAX_DELAY);
  notice = s;
  noticeUntil = millis() + 4000;
  version++;
  xSemaphoreGive(lock);
  Serial.println(s);
}

// --- Spotify Web API (network task only) ---

static WiFiClientSecure apiTls;  // kept open between requests: a TLS handshake takes about a second
static HTTPClient apiHttp;
static String accessToken;
static uint32_t tokenExpires = 0;
static uint32_t backoffUntil = 0;  // after a 429 from Spotify

static String urlEncode(const String &s) {
  String out;
  for (char c : s) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
      out += hex;
    }
  }
  return out;
}

static bool ensureToken() {
  if (accessToken.length() && (int32_t)(tokenExpires - millis()) > 60000) return true;
  apiTls.stop();  // two TLS sessions at once is more than this board's memory can hold
  WiFiClientSecure tls;
  tls.setCACert(SPOTIFY_ROOT_CAS);
  tls.setHandshakeTimeout(15);
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);
  http.begin(tls, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST("grant_type=refresh_token&refresh_token=" + urlEncode(refreshToken) +
                       "&client_id=" + urlEncode(clientId));
  String body = http.getString();
  http.end();
  if (code < 0) {
    char err[100];
    tls.lastError(err, sizeof(err));
    Serial.printf("token: %s; TLS: %s\n", http.errorToString(code).c_str(), err);
  }
  if (code != 200) {
    Serial.printf("token: HTTP %d %s\n", code, body.c_str());
    setStatus(code == 400 ? "Spotify sign-in expired.\nRun the setup again." : "Can't reach Spotify (" + String(code) + ")");
    accessToken = "";
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  accessToken = doc["access_token"].as<String>();
  tokenExpires = millis() + doc["expires_in"].as<uint32_t>() * 1000;
  const char *rotated = doc["refresh_token"];
  if (rotated && refreshToken != rotated) {  // Spotify may hand out a new one; the old one stops working
    refreshToken = rotated;
    prefs.putString("refresh_token", refreshToken);
  }
  return true;
}

// Calls the Web API. Returns the HTTP status, or a negative number if it never got that far.
static int api(const char *method, const String &path, String *body = nullptr) {
  if ((int32_t)(backoffUntil - millis()) > 0) return -2;
  if (!ensureToken()) return -1;
  static bool first = true;
  if (first) Serial.printf("signed in to Spotify; free heap %u\n", ESP.getFreeHeap());
  first = false;
  apiHttp.setReuse(true);
  apiHttp.setTimeout(8000);
  apiHttp.setConnectTimeout(10000);
  apiHttp.begin(apiTls, "https://api.spotify.com/v1" + path);
  apiHttp.addHeader("Authorization", "Bearer " + accessToken);
  static const char *keep[] = {"Retry-After"};
  apiHttp.collectHeaders(keep, 1);
  int code;
  if (strcmp(method, "GET") == 0) {
    code = apiHttp.GET();
  } else {
    apiHttp.addHeader("Content-Length", "0");  // Spotify rejects a PUT/POST without it
    code = apiHttp.sendRequest(method, (uint8_t *)nullptr, 0);
  }
  // Read the body so the connection can be reused, but not after 204: there is none, and with
  // no length given and the connection kept open, reading would wait for ever.
  String text = code > 0 && code != 204 && code != 304 ? apiHttp.getString() : String();
  if (code == 429) {
    int wait = max(1, (int)apiHttp.header("Retry-After").toInt());
    backoffUntil = millis() + wait * 1000;
    Serial.printf("rate limited for %d s\n", wait);
  }
  apiHttp.end();
  if (code == 401) accessToken = "";  // expired early; get a new one next time
  if (code < 0) {
    char err[100];
    apiTls.lastError(err, sizeof(err));
    Serial.printf("%s %s: %s; TLS: %s; free heap %u, largest block %u\n", method, path.c_str(),
                  HTTPClient::errorToString(code).c_str(), err, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    apiTls.stop();
  }
  if (body) *body = text;
  return code;
}

// --- album art ---

static int artW, artH;  // size of the decoded (scaled) image

// JPEGDEC hands over blocks of the decoded image; this samples them down to ART x ART.
static int artDraw(JPEGDRAW *d) {
  for (int dy = 0; dy < ART; dy++) {
    int sy = dy * artH / ART;
    if (sy < d->y || sy >= d->y + d->iHeight) continue;
    for (int dx = 0; dx < ART; dx++) {
      int sx = dx * artW / ART;
      if (sx < d->x || sx >= d->x + d->iWidth) continue;
      artPixels[dy * ART + dx] = d->pPixels[(sy - d->y) * d->iWidth + (sx - d->x)];
    }
  }
  return 1;
}

static void loadArt(const String &url) {
  bool ok = false;
  if (url.length()) {
    apiTls.stop();  // as in ensureToken()
    WiFiClientSecure tls;
    tls.setCACert(SPOTIFY_ROOT_CAS);
    HTTPClient http;
    http.useHTTP10(true);  // a plain body, not chunked, so it can be read straight off the socket
    http.setTimeout(8000);
    http.begin(tls, url);
    int code = http.GET();
    int len = http.getSize();
    // Borrowed only while a cover loads: TLS needs that memory the rest of the time.
    uint8_t *buf = code == 200 && len > 0 && (size_t)len <= ART_MAX ? (uint8_t *)malloc(len) : nullptr;
    if (buf) {
      WiFiClient *s = http.getStreamPtr();
      int got = 0;
      uint32_t start = millis();
      while (got < len && millis() - start < 10000) {
        int n = s->read(buf + got, len - got);
        if (n > 0) got += n; else delay(5);
      }
      if (got == len && jpeg.openRAM(buf, len, artDraw)) {
        int scale = 0, w = jpeg.getWidth(), h = jpeg.getHeight();
        while (scale < 3 && w >> (scale + 1) >= ART && h >> (scale + 1) >= ART) scale++;
        artW = w >> scale;
        artH = h >> scale;
        artValid = false;  // the loop must not draw a half-written image
        ok = jpeg.decode(0, 0, scale ? (1 << scale) : 0) == 1;  // JPEG_SCALE_HALF == 2, and so on
        jpeg.close();
      }
      free(buf);
    } else {
      Serial.printf("art: HTTP %d, %d bytes, largest free block %u\n", code, len, ESP.getMaxAllocHeap());
    }
    http.end();
  }
  artValid = ok;
  artVersion++;
}

// --- polling and commands ---

static String artUrlWanted, artUrlLoaded, likedChecked;

static void poll() {
  String body;
  int code = api("GET", "/me/player?additional_types=episode", &body);
  if (code == 204 || (code == 200 && body.length() == 0)) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (player.valid) version++;
    player.valid = false;
    status = "Nothing playing.\nStart Spotify on any device.";
    xSemaphoreGive(lock);
    return;
  }
  if (code != 200) {
    if (code > 0) Serial.printf("player: HTTP %d %s\n", code, body.substring(0, 200).c_str());
    if (code == 403) setStatus("Spotify refused: playback control\nneeds Spotify Premium.");
    return;
  }

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["shuffle_state"] = true;
  filter["repeat_state"] = true;
  filter["progress_ms"] = true;
  filter["device"]["name"] = true;
  JsonObject item = filter["item"].to<JsonObject>();
  item["uri"] = true;
  item["name"] = true;
  item["duration_ms"] = true;
  item["artists"][0]["name"] = true;
  item["album"]["name"] = true;
  item["album"]["images"][0]["url"] = true;
  item["album"]["images"][0]["width"] = true;
  item["show"]["name"] = true;  // podcast episodes have a show instead of an album
  item["images"][0]["url"] = true;
  item["images"][0]["width"] = true;
  JsonDocument doc;
  if (DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    Serial.printf("player: bad JSON (%s)\n", err.c_str());
    return;
  }
  JsonObject it = doc["item"];
  if (it.isNull()) {  // an ad, or a local file Spotify won't describe
    setStatus("Playing something Spotify\nwon't describe (an ad?)");
    xSemaphoreTake(lock, portMAX_DELAY);
    if (player.valid) version++;
    player.valid = false;
    xSemaphoreGive(lock);
    return;
  }

  String artists;
  for (JsonObject a : it["artists"].as<JsonArray>()) {
    if (artists.length()) artists += ", ";
    artists += a["name"].as<String>();
  }
  bool episode = it["show"].is<JsonObject>();
  JsonArray images = episode ? it["images"].as<JsonArray>() : it["album"]["images"].as<JsonArray>();
  String art;  // the smallest cover that is still at least ART pixels wide (sizes come largest first)
  for (JsonObject img : images) {
    if (!art.length() || img["width"].as<int>() >= ART) art = img["url"].as<String>();
  }

  Player p;
  p.valid = true;
  p.playing = doc["is_playing"];
  p.shuffle = doc["shuffle_state"];
  String rep = doc["repeat_state"] | "off";
  p.repeat = rep == "context" ? 1 : rep == "track" ? 2 : 0;
  p.uri = it["uri"].as<String>();
  p.title = it["name"].as<String>();
  p.artist = episode ? it["show"]["name"].as<String>() : artists;
  p.album = episode ? String() : it["album"]["name"].as<String>();
  p.device = doc["device"]["name"].as<String>();
  p.durationMs = it["duration_ms"];
  p.progressMs = doc["progress_ms"];
  p.at = millis();

  xSemaphoreTake(lock, portMAX_DELAY);
  p.liked = p.uri == player.uri ? player.liked : -1;
  bool changed = !player.valid || p.uri != player.uri || p.title != player.title || p.playing != player.playing ||
                 p.shuffle != player.shuffle || p.repeat != player.repeat || p.device != player.device ||
                 abs((int32_t)(p.progressMs - (player.progressMs + (player.playing ? p.at - player.at : 0)))) > 1500;
  player = p;
  if (changed) version++;
  xSemaphoreGive(lock);
  artUrlWanted = art;
}

static void checkLiked(const String &uri) {
  String body;
  int code = api("GET", "/me/library/contains?uris=" + urlEncode(uri), &body);
  if (code != 200) return;
  bool liked = body.indexOf("true") >= 0;  // the reply is [true] or [false]
  xSemaphoreTake(lock, portMAX_DELAY);
  if (player.uri == uri) {
    player.liked = liked;
    version++;
  }
  xSemaphoreGive(lock);
  likedChecked = uri;
}

static void run(const Action &a) {
  static const char *modes[] = {"off", "context", "track"};
  int code;
  switch (a.button) {
    case B_PLAY: code = api("PUT", a.value ? "/me/player/play" : "/me/player/pause"); break;
    case B_NEXT: code = api("POST", "/me/player/next"); break;
    case B_PREV: code = api("POST", "/me/player/previous"); break;
    case B_SHUFFLE: code = api("PUT", String("/me/player/shuffle?state=") + (a.value ? "true" : "false")); break;
    case B_REPEAT: code = api("PUT", String("/me/player/repeat?state=") + modes[a.value % 3]); break;
    case B_LIKE: code = api(a.value ? "PUT" : "DELETE", "/me/library?uris=" + urlEncode(a.uri)); break;
    default: return;
  }
  if (code >= 200 && code < 300) return;
  Serial.printf("button %d: HTTP %d\n", a.button, code);
  setNotice(code == 403 ? "Needs Spotify Premium" : code == 404 ? "No active Spotify device" : "Spotify didn't respond");
  if (a.button == B_LIKE) likedChecked = "";  // re-read the real state
}

// Guest networks often hold everything back until someone signs in on a web page, which this
// board can't do. A plain-HTTP check page tells: it answers 204 only on an open connection.
// Its Date header also sets the clock on networks that block time servers.
static bool internetReachable() {
  WiFiClient tcp;
  HTTPClient http;
  static const char *keep[] = {"Date"};
  http.collectHeaders(keep, 1);
  http.setTimeout(8000);
  http.begin(tcp, "http://connectivitycheck.gstatic.com/generate_204");
  int code = http.GET();
  String date = http.header("Date");
  http.end();
  Serial.printf("internet check: HTTP %d\n", code);
  if (code != 204) return false;
  struct tm t = {};
  if (time(nullptr) < 1700000000 && strptime(date.c_str(), "%a, %d %b %Y %H:%M:%S", &t)) {
    struct timeval tv = {mktime(&t), 0};  // the board's time zone is UTC, like the header
    settimeofday(&tv, nullptr);
    Serial.println("clock: set from the HTTP Date header");
  }
  return true;
}

static void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (status.indexOf("sign-in page") < 0) setStatus("Connecting to Wi-Fi\n" + wifiSsid);  // keep the advice up
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  for (int i = 0; i < 300 && WiFi.status() != WL_CONNECTED; i++) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    setStatus("Can't join Wi-Fi\n" + wifiSsid);
    WiFi.disconnect();
    delay(5000);
    return;
  }
  Serial.printf("Wi-Fi: %s, MAC %s\n", WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
  // Certificates are checked against the clock, which starts at 1970.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 100 && time(nullptr) < 1700000000; i++) delay(100);
  if (!internetReachable()) {
    setStatus("This Wi-Fi wants a sign-in page.\nRegister MAC " + WiFi.macAddress());
    delay(30000);
    WiFi.disconnect();  // rejoin, so a newly registered board gets its proper address
    return;
  }
  if (time(nullptr) < 1700000000) {
    setStatus("Can't get the time from\nthe internet; Spotify needs it.");
    delay(30000);
    return;
  }
  time_t now = time(nullptr);
  Serial.printf("clock: %s", ctime(&now));
  setStatus("Connecting to Spotify...");
}

static void networkTask(void *) {
  apiTls.setCACert(SPOTIFY_ROOT_CAS);
  apiTls.setHandshakeTimeout(15);
  uint32_t nextPoll = 0;
  for (;;) {
    connectWifi();
    if (WiFi.status() != WL_CONNECTED) continue;
    Action *a;
    if (xQueueReceive(actions, &a, pdMS_TO_TICKS(50)) == pdTRUE) {
      run(*a);
      delete a;
      nextPoll = millis() + 400;  // show what Spotify made of it
      continue;
    }
    if ((int32_t)(millis() - nextPoll) >= 0) {
      poll();
      xSemaphoreTake(lock, portMAX_DELAY);
      Player p = player;
      xSemaphoreGive(lock);
      nextPoll = millis() + (p.valid && p.playing ? 2000 : 4000);
      if (p.valid && p.playing && p.durationMs > p.progressMs) {  // catch the next song as it starts
        nextPoll = min(nextPoll, p.at + (p.durationMs - p.progressMs) + 800);
      }
      if (p.valid && p.uri != likedChecked) checkLiked(p.uri);
    }
    if (artUrlWanted != artUrlLoaded) {
      artUrlLoaded = artUrlWanted;
      loadArt(artUrlWanted);
    }
  }
}

// --- drawing (loop task only) ---

// The built-in fonts only cover ASCII, so accented Latin letters lose their accents and
// anything else becomes '?'.
static String toAscii(const String &s) {
  static const char *latin1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c < 0x80) {
      out += (char)c;
      continue;
    }
    uint32_t cp = 0;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i + 1 < s.length(); k++) cp = (cp << 6) | (s[++i] & 0x3F);
    if (cp >= 0xC0 && cp <= 0xFF) out += latin1[cp - 0xC0];
    else if (cp == 0x2018 || cp == 0x2019) out += '\'';
    else if (cp == 0x201C || cp == 0x201D) out += '"';
    else if (cp == 0x2013 || cp == 0x2014) out += '-';
    else if (cp == 0x2026) out += "...";
    else out += '?';
  }
  return out;
}

static String fitText(String s, int width) {
  if (tft.textWidth(s) <= width) return s;
  while (s.length() && tft.textWidth(s + "...") > width) s.remove(s.length() - 1);
  s.trim();
  return s + "...";
}

// Draws up to `lines` word-wrapped lines from y; returns the y below the last one.
static int drawWrapped(const String &text, int x, int y, int width, int lines, int lineH, uint16_t color) {
  tft.setTextColor(color);
  String rest = text;
  rest.trim();
  for (int n = 0; n < lines && rest.length(); n++) {
    String line;
    int cut = 0;
    while (cut < (int)rest.length()) {
      int sp = rest.indexOf(' ', cut + 1);
      if (sp < 0) sp = rest.length();
      if (line.length() && tft.textWidth(rest.substring(0, sp)) > width) break;
      line = rest.substring(0, sp);
      cut = sp;
    }
    rest = rest.substring(cut);
    rest.trim();
    if (n == lines - 1 && rest.length()) line = line + " " + rest;
    tft.drawString(fitText(line, width), x, y);
    y += lineH;
  }
  return y;
}

static void drawStatus(const String &msg) {
  tft.fillScreen(BG);
  tft.setTextColor(DIM);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSans9pt7b);
  int nl = msg.indexOf('\n');
  if (nl < 0) {
    tft.drawString(msg, W / 2, H / 2);
  } else {
    tft.drawString(msg.substring(0, nl), W / 2, H / 2 - 12);
    tft.drawString(msg.substring(nl + 1), W / 2, H / 2 + 12);
  }
  tft.setTextDatum(TL_DATUM);
}

static void drawText(const Player &p, const String &line) {
  tft.fillRect(TEXT_X, ART_Y, W - TEXT_X, ART, BG);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  int y = drawWrapped(toAscii(p.title), TEXT_X, ART_Y + 2, TEXT_W, 3, 22, FG) + 4;
  tft.setFreeFont(&FreeSans9pt7b);
  y = drawWrapped(toAscii(p.artist), TEXT_X, y, TEXT_W, 2, 20, FG);
  tft.setTextFont(2);
  if (p.album.length() && y < 112) {
    tft.setTextColor(DIM);
    tft.drawString(fitText(toAscii(p.album), TEXT_W), TEXT_X, y + 2);
  }
  tft.setTextColor(DIM);
  tft.drawString(fitText(toAscii(line), TEXT_W), TEXT_X, ART_Y + ART - 16);
}

static void drawArt() {
  if (artValid) {
    tft.pushImage(ART_X, ART_Y, ART, ART, artPixels);
  } else {
    tft.fillRect(ART_X, ART_Y, ART, ART, BG);
    tft.fillRoundRect(ART_X, ART_Y, ART, ART, 6, TRACK);
  }
}

static String clockText(uint32_t ms) {
  uint32_t s = ms / 1000;
  char buf[12];
  if (s >= 3600) snprintf(buf, sizeof(buf), "%u:%02u:%02u", s / 3600, s % 3600 / 60, s % 60);
  else snprintf(buf, sizeof(buf), "%u:%02u", s / 60, s % 60);
  return buf;
}

static void drawProgress(uint32_t pos, uint32_t len) {
  tft.fillRect(0, BAR_Y, W, 26, BG);
  tft.fillRoundRect(12, BAR_Y, W - 24, 6, 2, TRACK);
  if (!len) return;
  int filled = (int)((uint64_t)(W - 24) * min(pos, len) / len);
  if (filled > 0) tft.fillRoundRect(12, BAR_Y, max(6, filled), 6, 2, ACCENT);
  tft.setTextFont(2);
  tft.setTextColor(DIM, BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(clockText(pos), 12, BAR_Y + 9);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(clockText(len), W - 12, BAR_Y + 9);
  tft.setTextDatum(TL_DATUM);
}

static void thickLine(int x0, int y0, int x1, int y1, uint16_t c) {
  tft.drawLine(x0, y0, x1, y1, c);
  tft.drawLine(x0, y0 + 1, x1, y1 + 1, c);
}

static void drawButton(int b, const Player &p, bool pressed, bool visible) {
  int bw = W / N_BUTTONS, x0 = b * bw, cx = x0 + bw / 2, cy = (BTN_Y0 + BTN_Y1) / 2;
  tft.fillRect(x0, BTN_Y0 - 2, bw, H - BTN_Y0 + 2, BG);
  if (!visible) return;
  if (pressed) tft.fillRoundRect(x0 + 6, BTN_Y0, bw - 12, BTN_Y1 - BTN_Y0, 10, TRACK);
  uint16_t bg = pressed ? TRACK : BG;
  switch (b) {
    case B_PLAY:
      if (p.playing) {
        tft.fillRect(cx - 9, cy - 11, 7, 23, FG);
        tft.fillRect(cx + 3, cy - 11, 7, 23, FG);
      } else {
        tft.fillTriangle(cx - 8, cy - 12, cx - 8, cy + 12, cx + 12, cy, FG);
      }
      break;
    case B_PREV:
    case B_NEXT: {
      int s = b == B_PREV ? -1 : 1;
      for (int off : {-8, 2}) tft.fillTriangle(cx + s * off, cy - 9, cx + s * off, cy + 9, cx + s * (off + 10), cy, FG);
      tft.fillRect(cx + s * 12 - 1, cy - 9, 3, 19, FG);
      break;
    }
    case B_SHUFFLE: {
      uint16_t c = p.shuffle ? ACCENT : FG;
      thickLine(cx - 11, cy - 7, cx + 6, cy + 7, c);
      thickLine(cx - 11, cy + 7, cx + 6, cy - 7, c);
      tft.fillTriangle(cx + 12, cy + 7, cx + 5, cy + 2, cx + 5, cy + 12, c);
      tft.fillTriangle(cx + 12, cy - 7, cx + 5, cy - 12, cx + 5, cy - 2, c);
      if (p.shuffle) tft.fillCircle(cx, cy + 16, 2, c);
      break;
    }
    case B_REPEAT: {
      uint16_t c = p.repeat ? ACCENT : FG;
      tft.drawRoundRect(cx - 11, cy - 7, 23, 15, 5, c);
      tft.drawRoundRect(cx - 10, cy - 6, 21, 13, 4, c);
      tft.fillRect(cx - 8, cy - 9, 4, 5, bg);  // gap behind the arrowhead
      tft.fillTriangle(cx - 2, cy - 6, cx - 7, cy - 11, cx - 7, cy - 1, c);
      if (p.repeat == 2) {  // a small "1": repeat this song
        tft.fillRect(cx, cy - 3, 2, 7, c);
        thickLine(cx - 2, cy - 2, cx, cy - 4, c);
      }
      if (p.repeat) tft.fillCircle(cx, cy + 16, 2, c);
      break;
    }
    case B_LIKE: {
      int r = 6;
      uint16_t c = p.liked == 1 ? ACCENT : FG;
      if (p.liked == 1) {
        tft.fillCircle(cx - r, cy - 4, r, c);
        tft.fillCircle(cx + r, cy - 4, r, c);
        tft.fillTriangle(cx - 2 * r, cy - 2, cx + 2 * r, cy - 2, cx, cy + 2 * r, c);
      } else {
        tft.drawCircle(cx - r, cy - 4, r, c);
        tft.drawCircle(cx + r, cy - 4, r, c);
        tft.drawCircle(cx - r, cy - 4, r - 1, c);
        tft.drawCircle(cx + r, cy - 4, r - 1, c);
        tft.fillRect(cx - 2 * r, cy - 3, 4 * r + 1, r, bg);  // keep only the top of the circles
        tft.fillRect(cx - 1, cy - 4 - r + 1, 3, r, bg);
        thickLine(cx - 2 * r, cy - 4, cx, cy + 2 * r - 1, c);
        thickLine(cx, cy + 2 * r - 1, cx + 2 * r, cy - 4, c);
      }
      break;
    }
  }
}

// --- touch ---

static uint8_t pressedButton = B_NONE;

static uint8_t buttonAt(int x, int y) {
  if (y < BTN_Y0 - 8) return B_NONE;
  return constrain(x * N_BUTTONS / W, 0, N_BUTTONS - 1);
}

// Shows the result of a tap straight away; the network task then tells Spotify.
static void press(uint8_t b) {
  Action *a = nullptr;
  xSemaphoreTake(lock, portMAX_DELAY);
  Player &p = player;
  if (p.valid && (b != B_LIKE || p.liked >= 0)) {
    uint32_t now = millis();
    a = new Action{b, 0, p.uri};
    switch (b) {
      case B_PLAY:
        p.progressMs += p.playing ? now - p.at : 0;
        p.at = now;
        a->value = p.playing = !p.playing;
        break;
      case B_SHUFFLE: a->value = p.shuffle = !p.shuffle; break;
      case B_REPEAT: a->value = p.repeat = (p.repeat + 1) % 3; break;
      case B_LIKE: a->value = p.liked = !p.liked; break;
    }
    version++;
  }
  xSemaphoreGive(lock);
  if (a && xQueueSend(actions, &a, 0) != pdTRUE) delete a;  // queue full: drop the tap
}

static void pollTouch() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 25) return;
  lastPoll = millis();
  bool down = touch.tirqTouched() && touch.touched();
  if (down) {
    TS_Point pt = touch.getPoint();
    int x = constrain(map(pt.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, W - 1), 0, W - 1);
    int y = constrain(map(pt.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, H - 1), 0, H - 1);
    if (pressedButton == B_NONE) {
      pressedButton = buttonAt(x, y);
      if (pressedButton != B_NONE) {
        xSemaphoreTake(lock, portMAX_DELAY);
        version++;  // draw the highlight
        xSemaphoreGive(lock);
      }
    }
  } else if (pressedButton != B_NONE) {
    uint8_t b = pressedButton;
    pressedButton = B_NONE;
    press(b);
  }
}

// --- setup over USB ---

static void handleCommand(String line) {
  line.trim();
  if (line.startsWith("SET ")) {
    int eq = line.indexOf('=');
    String key = line.substring(4, eq);
    static const char *allowed[] = {"wifi_ssid", "wifi_pass", "client_id", "refresh_token"};
    for (const char *k : allowed) {
      if (eq > 4 && key == k) {
        prefs.putString(k, line.substring(eq + 1));
        Serial.println("OK");
        return;
      }
    }
    Serial.println("ERR unknown setting");
  } else if (line == "INFO") {
    Serial.printf("wifi_ssid=%s wifi_pass=%s client_id=%s refresh_token=%s\n",
                  prefs.isKey("wifi_ssid") ? "set" : "missing", prefs.isKey("wifi_pass") ? "set" : "missing",
                  prefs.isKey("client_id") ? "set" : "missing", prefs.isKey("refresh_token") ? "set" : "missing");
    Serial.println("OK");
  } else if (line == "CLEAR") {
    prefs.clear();
    Serial.println("OK");
  } else if (line == "REBOOT") {
    Serial.println("OK");
    Serial.flush();
    ESP.restart();
  } else if (line.length()) {
    Serial.println("ERR unknown command");
  }
}

static void readSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      handleCommand(line);
      line = "";
    } else if (line.length() < 1024) {
      line += c;
    }
  }
}

// --- main ---

void setup() {
  Serial.begin(SETUP_BAUD);
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);  // JPEGDEC emits little-endian RGB565
  BG = tft.color565(12, 14, 20);
  FG = tft.color565(235, 238, 245);
  DIM = tft.color565(140, 146, 160);
  TRACK = tft.color565(40, 44, 56);
  ACCENT = tft.color565(30, 215, 96);
  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);

  prefs.begin("cyd-spotify");
  wifiSsid = prefs.getString("wifi_ssid");
  wifiPass = prefs.getString("wifi_pass");
  clientId = prefs.getString("client_id");
  refreshToken = prefs.getString("refresh_token");

  lock = xSemaphoreCreateMutex();
  actions = xQueueCreate(8, sizeof(Action *));
  Serial.println("CYD-STANDALONE READY");
  if (!wifiSsid.length() || !clientId.length() || !refreshToken.length()) {
    status = "Not set up yet. On a PC, run\ncyd_spotify.py --setup-standalone";
    return;  // the loop still answers setup commands
  }
  xTaskCreatePinnedToCore(networkTask, "net", 20480, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t drawnVersion = 0, drawnArt = UINT32_MAX, drawnSecond = UINT32_MAX;
  static Player shown;
  static bool shownValid = false, noticeShown = false;
  static uint8_t shownPressed = B_NONE;
  static int8_t shownLiked = -2;

  readSerial();
  pollTouch();

  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = version;
  Player p = (v != drawnVersion) ? player : shown;
  String st = status, note = notice;
  bool noticeOn = note.length() && (int32_t)(noticeUntil - millis()) > 0;
  xSemaphoreGive(lock);

  if (!p.valid) {
    if (v != drawnVersion || shownValid) drawStatus(st);
    shown = p;
    shownValid = false;
    drawnVersion = v;
    delay(10);
    return;
  }

  bool full = !shownValid;
  if (full) tft.fillScreen(BG);
  if (full || artVersion != drawnArt) {
    drawnArt = artVersion;
    drawArt();
  }
  if (full || p.uri != shown.uri || p.title != shown.title || p.artist != shown.artist ||
      p.device != shown.device || noticeOn != noticeShown) {
    drawText(p, noticeOn ? note : p.device);
    noticeShown = noticeOn;
  }
  uint32_t pos = p.progressMs + (p.playing ? millis() - p.at : 0);
  if (p.durationMs) pos = min(pos, p.durationMs);
  if (full || pos / 1000 != drawnSecond || p.durationMs != shown.durationMs) {
    drawnSecond = pos / 1000;
    drawProgress(pos, p.durationMs);
  }
  for (int b = 0; b < N_BUTTONS; b++) {
    bool changed = full || (pressedButton == b) != (shownPressed == b);
    switch (b) {
      case B_PLAY: changed |= p.playing != shown.playing; break;
      case B_SHUFFLE: changed |= p.shuffle != shown.shuffle; break;
      case B_REPEAT: changed |= p.repeat != shown.repeat; break;
      case B_LIKE: changed |= p.liked != shownLiked; break;
    }
    if (changed) drawButton(b, p, pressedButton == b, b != B_LIKE || p.liked >= 0);
  }
  shown = p;
  shownValid = true;
  shownPressed = pressedButton;
  shownLiked = p.liked;
  drawnVersion = v;
  delay(10);
}
