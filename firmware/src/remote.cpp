// ESP32-2432S028 (CYD) as a Bluetooth media remote. It pairs as a Bluetooth LE media-key
// device (HID consumer control, like the media keys on a keyboard), so the phone or computer
// keeps playing through its own speaker or headphones. Buttons: previous, play/pause, next,
// volume down and up.
//
// A remote can't receive song details by itself. The companion Android app (android/ in this
// repository) fills that gap: it reads what's playing on the phone and writes it to a second
// service on the same Bluetooth LE connection. While it does, the screen shows the title,
// artist, album, cover and progress above smaller buttons; without it, just the big buttons.
//
// Now-playing service: one characteristic, written by the app. Every write is one packet whose
// first byte is its type; numbers are little-endian.
//   0x01 song    state (0 nothing, 1 paused, 2 playing), volume % (255 unknown),
//                position ms (u32), duration ms (u32, 0 unknown),
//                then title, artist, album and player name, UTF-8, each ending in a 0 byte
//   0x02 cover   id (u32), JPEG length (u32, 0: no cover)
//   0x03 piece   id (u32), offset (u32), JPEG bytes; the cover shows once all have arrived
//
// Firmware info: a readable characteristic, "id=<FW_ID>;flavor=<FW_FLAVOR>;board=<FW_BOARD>".
//
// Firmware update: a characteristic only a paired (encrypted) link can write, with packets
// like the now-playing ones; the CYD answers each step in a notification on it.
//   0x10 begin   size (u32), SHA-256 of the image (32 bytes)      -> "ready" or "err:<why>"
//   0x11 data    offset (u32), image bytes, in order; may come unacknowledged, so the CYD
//                reports "ack:<bytes so far>" every UPDATE_ACK_EVERY bytes and at the end,
//                and the app keeps no more than a few of those ahead
//   0x12 end     checks the SHA-256 and the image                 -> "done" (then restarts)
//                                                                     or "err:<why>"
// A problem part-way also sends "err:<why>"; the running firmware stays until "done".
//
// Serial (SETUP_BAUD): a log.
//
// Bluetooth callbacks run in the Bluetooth task; the Arduino loop draws and reads the
// touchscreen. They share `song` and the finished cover under `lock`.

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <JPEGDEC.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#ifndef SETUP_BAUD
#define SETUP_BAUD 115200
#endif
#ifndef BT_NAME
#define BT_NAME "CYD Remote"
#endif
// Set by fw_info.py.
#ifndef FW_ID
#define FW_ID "unknown"
#endif
#ifndef FW_FLAVOR
#define FW_FLAVOR "public"
#endif
#ifndef FW_BOARD
#define FW_BOARD "unknown"
#endif

// Must match android/app/src/main/java/com/cyd/nowplaying/Protocol.kt.
#define NOW_PLAYING_SERVICE "8d5f0001-3c4b-4f1e-9a57-6b1c2d0e7a10"
#define NOW_PLAYING_CHAR "8d5f0002-3c4b-4f1e-9a57-6b1c2d0e7a10"
#define FIRMWARE_INFO_CHAR "8d5f0003-3c4b-4f1e-9a57-6b1c2d0e7a10"
#define FIRMWARE_UPDATE_CHAR "8d5f0004-3c4b-4f1e-9a57-6b1c2d0e7a10"

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

// Buttons only (no app):
static const int TOP_H = 44;                  // status bar
static const int ROW1_Y0 = 52, ROW1_Y1 = 140;  // previous, play/pause, next
static const int ROW2_Y0 = 148, ROW2_Y1 = 232; // volume down, volume up

// Now playing (the app is sending song details), laid out like the Bluetooth speaker version:
static const int ART = 128, ART_X = 12, ART_Y = 12;
static const int TEXT_X = ART_X + ART + 12, TEXT_W = W - TEXT_X - 12;
static const int BAR_Y = 156;
static const int BTN_Y0 = 186, BTN_Y1 = 234;
static const size_t ART_MAX = 32768;  // the app sends 128x128 JPEGs, usually under 10 KB

enum Button : uint8_t { B_PREV, B_PLAY, B_NEXT, B_VOL_DOWN, B_VOL_UP, N_BUTTONS, B_NONE = 255 };

// HID consumer-page usages (USB HID Usage Tables, section 15).
static const uint16_t USAGE[N_BUTTONS] = {0xB6, 0xCD, 0xB5, 0xEA, 0xE9};

static TFT_eSPI tft;
static JPEGDEC jpeg;
static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
static uint16_t BG, FG, DIM, TRACK, ACCENT;

// One 16-bit consumer usage per report; 0 means nothing is pressed.
static const uint8_t REPORT_MAP[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array, Absolute)
    0xC0,              // End Collection
};

// --- state shared between the Bluetooth task and the loop (guarded by `lock`) ---

struct Song {
  bool fromApp = false;  // the app has sent song details on this connection
  bool playing = false;
  uint8_t volume = 255;  // percent, 255 unknown
  String title, artist, album, source;
  uint32_t durationMs = 0, progressMs = 0, at = 0;  // progressMs as read at millis() == at
};

static SemaphoreHandle_t lock;
static Song song;
static uint32_t version = 1;  // bumped whenever the screen should be redrawn

// A cover arrives in pieces in the Bluetooth task and is decoded by the loop.
static uint8_t *artBuf = nullptr;  // being received (Bluetooth task only)
static size_t artLen = 0, artTotal = 0;
static uint32_t artId = 0;
static uint8_t *artReady = nullptr;  // complete, waiting to be decoded (guarded by `lock`)
static size_t artReadyLen = 0;
static bool artClear = false;     // the song has no cover: show the placeholder
static uint32_t artVersion = 0;   // bumped when the loop should redraw the art area

static uint16_t artPixels[ART * ART];
static bool artValid = false;  // loop only

static BLEHIDDevice *hid;
static BLECharacteristic *input;
static volatile bool connected = false;
static String notice;  // shown briefly in the status line (loop only)
static uint32_t noticeUntil = 0;
static BLECharacteristic *updateChar;
static volatile int8_t updatePercent = -1;  // a firmware update's progress; -1: none running
static volatile bool updateDone = false;    // installed: the loop restarts into it

static void bump() {
  xSemaphoreTake(lock, portMAX_DELAY);
  version++;
  xSemaphoreGive(lock);
}

// --- now-playing service (Bluetooth task) ---

static uint32_t readU32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// The next 0-terminated string from p, or what's left of the packet without one.
static String takeString(const uint8_t *&p, const uint8_t *end) {
  const uint8_t *s = p;
  while (p < end && *p) p++;
  String out;
  out.concat((const char *)s, p - s);
  if (p < end) p++;
  return out;
}

static void dropArt() {
  free(artBuf);
  artBuf = nullptr;
  artLen = artTotal = 0;
}

static void showNoArt() {
  xSemaphoreTake(lock, portMAX_DELAY);
  free(artReady);
  artReady = nullptr;
  artClear = true;
  artVersion++;
  xSemaphoreGive(lock);
}

static void onSong(const uint8_t *d, size_t n) {
  if (n < 11) return;
  const uint8_t *p = d + 11, *end = d + n;
  Song s;
  s.fromApp = true;
  s.playing = d[1] == 2;
  s.volume = d[2];
  s.progressMs = readU32(d + 3);
  s.durationMs = readU32(d + 7);
  s.at = millis();
  s.title = takeString(p, end);
  s.artist = takeString(p, end);
  s.album = takeString(p, end);
  s.source = takeString(p, end);
  xSemaphoreTake(lock, portMAX_DELAY);
  song = s;
  version++;
  xSemaphoreGive(lock);
}

static void onCover(const uint8_t *d, size_t n) {
  if (n < 9) return;
  dropArt();
  artId = readU32(d + 1);
  size_t total = readU32(d + 5);
  if (total == 0 || total > ART_MAX) {
    if (total) Serial.printf("art: %u bytes is too big\n", (unsigned)total);
    showNoArt();
    return;
  }
  artBuf = (uint8_t *)malloc(total);
  if (artBuf) artTotal = total;
}

static void onPiece(const uint8_t *d, size_t n) {
  if (n < 9 || !artBuf || readU32(d + 1) != artId) return;  // an older cover's last pieces
  size_t offset = readU32(d + 5), len = n - 9;
  if (offset != artLen || artLen + len > artTotal) {
    Serial.println("art: pieces out of order, dropped");
    dropArt();
    return;
  }
  memcpy(artBuf + artLen, d + 9, len);
  artLen += len;
  if (artLen < artTotal) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  free(artReady);
  artReady = artBuf;
  artReadyLen = artLen;
  artClear = false;
  artVersion++;
  xSemaphoreGive(lock);
  artBuf = nullptr;
  artLen = artTotal = 0;
}

class NowPlayingWrites : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    const uint8_t *d = (const uint8_t *)v.c_str();
    size_t n = v.length();
    if (!n) return;
    switch (d[0]) {
      case 0x01: onSong(d, n); break;
      case 0x02: onCover(d, n); break;
      case 0x03: onPiece(d, n); break;
    }
  }
};

// --- firmware update (Bluetooth task) ---

static esp_ota_handle_t otaHandle = 0;  // 0: no update running
static const esp_partition_t *otaPartition = nullptr;
static size_t otaSize = 0, otaGot = 0, otaAcked = 0;
static const size_t UPDATE_ACK_EVERY = 8192;
static uint8_t otaSha[32];
static mbedtls_sha256_context otaShaCtx;

static void updateReply(const char *msg) {
  updateChar->setValue((uint8_t *)msg, strlen(msg));
  updateChar->notify();
  Serial.printf("update: %s\n", msg);
}

// Drops a part-written update; the running firmware stays.
static void updateStop() {
  if (otaHandle) {
    esp_ota_abort(otaHandle);
    mbedtls_sha256_free(&otaShaCtx);
    otaHandle = 0;
  }
  updatePercent = -1;
  bump();
}

static void updateFail(const char *why) {
  char msg[80];
  snprintf(msg, sizeof(msg), "err:%s", why);
  updateReply(msg);
  updateStop();
}

static void onUpdateBegin(const uint8_t *d, size_t n) {
  updateStop();  // a new begin replaces an unfinished update
  if (n < 37) return updateFail("bad begin");
  otaSize = readU32(d + 1);
  memcpy(otaSha, d + 5, 32);
  otaPartition = esp_ota_get_next_update_partition(nullptr);
  if (!otaPartition) return updateFail("no room for an update: flash this firmware over USB once");
  if (otaSize == 0 || otaSize > otaPartition->size) return updateFail("image too big");
  // Sequential writes erase as they go: erasing the whole slot first would hold up Bluetooth.
  esp_err_t e = esp_ota_begin(otaPartition, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle);
  if (e != ESP_OK) {
    otaHandle = 0;
    return updateFail(esp_err_to_name(e));
  }
  mbedtls_sha256_init(&otaShaCtx);
  mbedtls_sha256_starts(&otaShaCtx, 0);
  otaGot = otaAcked = 0;
  updatePercent = 0;
  bump();
  updateReply("ready");
}

static void onUpdateData(const uint8_t *d, size_t n) {
  if (!otaHandle) return;  // after an error the rest of the image is ignored
  if (n < 5) return updateFail("bad data");
  size_t offset = readU32(d + 1), len = n - 5;
  if (offset != otaGot || otaGot + len > otaSize) return updateFail("pieces out of order");
  if (offset == 0 && (len == 0 || d[5] != 0xE9)) return updateFail("not a firmware image");
  esp_err_t e = esp_ota_write(otaHandle, d + 5, len);
  if (e != ESP_OK) return updateFail(esp_err_to_name(e));
  mbedtls_sha256_update(&otaShaCtx, d + 5, len);
  otaGot += len;
  if (otaGot - otaAcked >= UPDATE_ACK_EVERY || otaGot == otaSize) {
    otaAcked = otaGot;
    char msg[20];
    snprintf(msg, sizeof(msg), "ack:%u", (unsigned)otaGot);
    updateChar->setValue((uint8_t *)msg, strlen(msg));
    updateChar->notify();
  }
  int8_t percent = otaGot * 100 / otaSize;
  if (percent != updatePercent) {
    updatePercent = percent;
    bump();
  }
}

static void onUpdateEnd() {
  if (!otaHandle) return;
  if (otaGot != otaSize) return updateFail("image incomplete");
  uint8_t sha[32];
  mbedtls_sha256_finish(&otaShaCtx, sha);
  if (memcmp(sha, otaSha, sizeof(sha))) return updateFail("checksum doesn't match");
  esp_ota_handle_t h = otaHandle;
  otaHandle = 0;
  mbedtls_sha256_free(&otaShaCtx);
  esp_err_t e = esp_ota_end(h);  // checks the image too; frees `h` either way
  if (e == ESP_OK) e = esp_ota_set_boot_partition(otaPartition);
  if (e != ESP_OK) return updateFail(esp_err_to_name(e));
  updateDone = true;
  bump();
  updateReply("done");
}

class UpdateWrites : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    const uint8_t *d = (const uint8_t *)v.c_str();
    size_t n = v.length();
    if (!n) return;
    switch (d[0]) {
      case 0x10: onUpdateBegin(d, n); break;
      case 0x11: onUpdateData(d, n); break;
      case 0x12: onUpdateEnd(); break;
    }
  }
};

// --- Bluetooth ---

class ServerEvents : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    connected = true;
    bump();
    Serial.println("ble: connected");
  }
  void onDisconnect(BLEServer *) override {
    connected = false;
    if (otaHandle) {
      Serial.println("update: link lost, dropped");
      updateStop();
    }
    dropArt();
    showNoArt();
    xSemaphoreTake(lock, portMAX_DELAY);
    song = Song();  // back to the big buttons until the app speaks again
    version++;
    xSemaphoreGive(lock);
    Serial.println("ble: disconnected");
    BLEDevice::startAdvertising();  // visible again for this or another device
  }
  void onMtuChanged(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
    Serial.printf("ble: MTU %u\n", param->mtu.mtu);
  }
};

class SecurityEvents : public BLESecurityCallbacks {
  bool onSecurityRequest() override { return true; }
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }  // no keyboard here: accept
  void onAuthenticationComplete(esp_ble_auth_cmpl_t r) override {
    Serial.printf("ble: pairing %s\n", r.success ? "done" : "failed");
  }
};

static void startBle() {
  BLEDevice::init(BT_NAME);
  BLEDevice::setMTU(517);  // the app sends covers in pieces of up to 500 bytes
  BLESecurity *security = new BLESecurity();
  security->setCapability(ESP_IO_CAP_NONE);  // "just works" pairing, no code to type
  security->setAuthenticationMode(true, false, true);  // bond, no MITM, secure connections
  BLEDevice::setSecurityCallbacks(new SecurityEvents());

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerEvents());
  hid = new BLEHIDDevice(server);
  hid->manufacturer()->setValue("CYD");
  hid->pnp(0x02, 0x303A, 0x4001, 0x0100);  // USB vendor ID of Espressif
  hid->hidInfo(0x00, 0x01);
  hid->reportMap((uint8_t *)REPORT_MAP, sizeof(REPORT_MAP));
  input = hid->inputReport(1);
  hid->setBatteryLevel(100);
  hid->startServices();

  BLEService *np = server->createService(NOW_PLAYING_SERVICE);
  BLECharacteristic *npChar = np->createCharacteristic(
      NOW_PLAYING_CHAR, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  npChar->setCallbacks(new NowPlayingWrites());
  BLECharacteristic *info = np->createCharacteristic(FIRMWARE_INFO_CHAR, BLECharacteristic::PROPERTY_READ);
  info->setValue("id=" FW_ID ";flavor=" FW_FLAVOR ";board=" FW_BOARD);
  updateChar = np->createCharacteristic(
      FIRMWARE_UPDATE_CHAR,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  updateChar->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);  // paired phones only
  updateChar->addDescriptor(new BLE2902());
  updateChar->setCallbacks(new UpdateWrites());
  np->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x0180);  // generic remote control
  adv->addServiceUUID(hid->hidService()->getUUID());
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("ble: \"%s\" %s, visible for pairing\n", BT_NAME, BLEDevice::getAddress().toString().c_str());
}

// A key press: the usage, then 0 to release it.
static void sendUsage(uint16_t usage) {
  uint8_t report[2] = {(uint8_t)usage, (uint8_t)(usage >> 8)};
  input->setValue(report, sizeof(report));
  input->notify();
}

static void tap(uint8_t b) {
  if (!connected) {
    notice = "Not connected yet";
    noticeUntil = millis() + 2000;
    bump();
    return;
  }
  if (b == B_PLAY) {  // show the change straight away; the app confirms it
    xSemaphoreTake(lock, portMAX_DELAY);
    if (song.fromApp) {
      uint32_t now = millis();
      song.progressMs += song.playing ? now - song.at : 0;
      song.at = now;
      song.playing = !song.playing;
      version++;
    }
    xSemaphoreGive(lock);
  }
  sendUsage(USAGE[b]);
  delay(10);
  sendUsage(0);
}

// --- album art (loop only) ---

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

static void decodeArt(uint8_t *buf, size_t len) {
  artValid = false;
  if (jpeg.openRAM(buf, len, artDraw)) {
    int scale = 0, w = jpeg.getWidth(), h = jpeg.getHeight();
    while (scale < 3 && w >> (scale + 1) >= ART && h >> (scale + 1) >= ART) scale++;
    artW = w >> scale;
    artH = h >> scale;
    artValid = jpeg.decode(0, 0, scale ? (1 << scale) : 0) == 1;  // JPEG_SCALE_HALF == 2, and so on
    jpeg.close();
  }
  if (!artValid) Serial.printf("art: couldn't decode %u bytes\n", (unsigned)len);
}

// --- drawing (loop only) ---

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

// Buttons only:

static void buttonRect(int b, int &x0, int &y0, int &w, int &h) {
  if (b <= B_NEXT) {
    w = W / 3;
    x0 = b * w;
    y0 = ROW1_Y0;
    h = ROW1_Y1 - ROW1_Y0;
  } else {
    w = W / 2;
    x0 = (b - B_VOL_DOWN) * w;
    y0 = ROW2_Y0;
    h = ROW2_Y1 - ROW2_Y0;
  }
}

static void drawStatus(bool noticeOn) {
  tft.fillRect(0, 0, W, TOP_H, BG);
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  if (noticeOn) {
    tft.setTextColor(FG);
    tft.drawString(notice, 12, TOP_H / 2);
  } else if (connected) {
    tft.fillCircle(18, TOP_H / 2, 5, ACCENT);
    tft.setTextColor(FG);
    tft.drawString("Connected", 32, TOP_H / 2);
  } else {
    tft.setTextColor(DIM);
    tft.drawString("Pair with", 12, TOP_H / 2);
    tft.setTextColor(ACCENT);
    tft.drawString(BT_NAME, 12 + tft.textWidth("Pair with "), TOP_H / 2);
  }
  tft.setTextDatum(TL_DATUM);
}

static void drawBigButton(int b, bool pressed) {
  int x0, y0, w, h;
  buttonRect(b, x0, y0, w, h);
  int cx = x0 + w / 2, cy = y0 + h / 2;
  tft.fillRect(x0, y0, w, h, BG);
  tft.fillRoundRect(x0 + 6, y0, w - 12, h, 14, pressed ? DIM : TRACK);
  uint16_t c = pressed ? BG : FG;
  switch (b) {
    case B_PREV:
    case B_NEXT: {
      int s = b == B_PREV ? -1 : 1;
      for (int off : {-14, 2}) tft.fillTriangle(cx + s * off, cy - 15, cx + s * off, cy + 15, cx + s * (off + 16), cy, c);
      tft.fillRect(cx + s * 19 - 2, cy - 15, 5, 31, c);
      break;
    }
    case B_PLAY:  // without the app the remote can't tell which, so both
      tft.fillTriangle(cx - 22, cy - 16, cx - 22, cy + 16, cx + 2, cy, c);
      tft.fillRect(cx + 8, cy - 16, 7, 33, c);
      tft.fillRect(cx + 20, cy - 16, 7, 33, c);
      break;
    case B_VOL_DOWN:
    case B_VOL_UP:
      tft.fillRect(cx - 22, cy - 8, 10, 17, c);  // speaker
      tft.fillTriangle(cx - 14, cy, cx - 1, cy - 16, cx - 1, cy + 16, c);
      tft.fillRect(cx + 6, cy - 2, 18, 5, c);  // minus
      if (b == B_VOL_UP) tft.fillRect(cx + 13, cy - 9, 5, 19, c);  // plus
      break;
  }
}

// Now playing:

static void drawText(const Song &s, const String &line) {
  tft.fillRect(TEXT_X, ART_Y, W - TEXT_X, ART, BG);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  String title = s.title.length() ? s.title : String(s.playing ? "Playing" : "Nothing playing");
  int y = drawWrapped(toAscii(title), TEXT_X, ART_Y + 2, TEXT_W, 3, 22, FG) + 4;
  tft.setFreeFont(&FreeSans9pt7b);
  y = drawWrapped(toAscii(s.artist), TEXT_X, y, TEXT_W, 2, 20, FG);
  tft.setTextFont(2);
  if (s.album.length() && y < 112) {
    tft.setTextColor(DIM);
    tft.drawString(fitText(toAscii(s.album), TEXT_W), TEXT_X, y + 2);
  }
  tft.setTextColor(DIM);
  tft.drawString(fitText(toAscii(line), TEXT_W), TEXT_X, ART_Y + ART - 16);
}

// No cover: a tile with a music note.
static void drawPlaceholder() {
  tft.fillRect(ART_X, ART_Y, ART, ART, BG);
  tft.fillRoundRect(ART_X, ART_Y, ART, ART, 6, TRACK);
  int cx = ART_X + ART / 2, cy = ART_Y + ART / 2;
  tft.fillEllipse(cx - 18, cy + 22, 12, 9, DIM);
  tft.fillEllipse(cx + 18, cy + 14, 12, 9, DIM);
  tft.fillRect(cx - 9, cy - 28, 5, 50, DIM);
  tft.fillRect(cx + 27, cy - 36, 5, 50, DIM);
  for (int i = 0; i < 8; i++) tft.drawLine(cx - 9, cy - 28 + i, cx + 31, cy - 36 + i, DIM);
}

static void drawArt() {
  if (artValid) tft.pushImage(ART_X, ART_Y, ART, ART, artPixels);
  else drawPlaceholder();
}

static String clockText(uint32_t ms) {
  uint32_t s = ms / 1000;
  char buf[12];
  if (s >= 3600) snprintf(buf, sizeof(buf), "%u:%02u:%02u", s / 3600, s % 3600 / 60, s % 60);
  else snprintf(buf, sizeof(buf), "%u:%02u", s / 60, s % 60);
  return buf;
}

// The progress bar, with the phone's volume in the middle of the line below it.
static void drawProgress(uint32_t pos, uint32_t len, uint8_t vol) {
  tft.fillRect(0, BAR_Y, W, 26, BG);
  tft.fillRoundRect(12, BAR_Y, W - 24, 6, 2, TRACK);
  tft.setTextFont(2);
  tft.setTextColor(DIM, BG);
  if (len) {
    int filled = (int)((uint64_t)(W - 24) * min(pos, len) / len);
    if (filled > 0) tft.fillRoundRect(12, BAR_Y, max(6, filled), 6, 2, ACCENT);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(clockText(pos), 12, BAR_Y + 9);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(clockText(len), W - 12, BAR_Y + 9);
  }
  if (vol <= 100) {
    tft.setTextDatum(TC_DATUM);
    tft.drawString(String("Volume ") + vol + "%", W / 2, BAR_Y + 9);
  }
  tft.setTextDatum(TL_DATUM);
}

static void drawSmallButton(int b, bool playing, bool pressed) {
  int bw = W / N_BUTTONS, x0 = b * bw, cx = x0 + bw / 2, cy = (BTN_Y0 + BTN_Y1) / 2;
  tft.fillRect(x0, BTN_Y0 - 2, bw, H - BTN_Y0 + 2, BG);
  if (pressed) tft.fillRoundRect(x0 + 6, BTN_Y0, bw - 12, BTN_Y1 - BTN_Y0, 10, TRACK);
  switch (b) {
    case B_PLAY:
      if (playing) {
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
    case B_VOL_DOWN:
    case B_VOL_UP:
      tft.fillRect(cx - 14, cy - 5, 6, 11, FG);  // speaker
      tft.fillTriangle(cx - 9, cy, cx - 1, cy - 10, cx - 1, cy + 10, FG);
      tft.fillRect(cx + 3, cy - 1, 11, 3, FG);  // minus
      if (b == B_VOL_UP) tft.fillRect(cx + 7, cy - 5, 3, 11, FG);  // plus
      break;
  }
}

// Firmware update: replaces the whole screen while it runs.
static void drawUpdate(int percent, bool done, bool full) {
  if (full) {
    tft.fillScreen(BG);
    tft.fillRoundRect(40, 108, W - 80, 8, 3, TRACK);
  }
  tft.setTextDatum(MC_DATUM);
  tft.fillRect(0, 64, W, 32, BG);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(FG);
  tft.drawString(done ? "Update installed" : String("Updating firmware ") + percent + "%", W / 2, 80);
  int filled = (W - 80) * percent / 100;
  if (filled > 0) tft.fillRoundRect(40, 108, max(8, filled), 8, 3, ACCENT);
  tft.fillRect(0, 136, W, 32, BG);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(DIM);
  tft.drawString(done ? "Restarting..." : "Keep the phone nearby", W / 2, 152);
  tft.setTextDatum(TL_DATUM);
}

// --- touch ---

static bool nowPlayingLayout = false;  // which layout is on screen (loop only)
static uint8_t pressedButton = B_NONE;
static uint32_t pressedSince = 0, lastRepeat = 0;

static uint8_t buttonAt(int x, int y) {
  if (nowPlayingLayout) {
    if (y < BTN_Y0 - 8) return B_NONE;
    return constrain(x * N_BUTTONS / W, 0, N_BUTTONS - 1);
  }
  for (int b = 0; b < N_BUTTONS; b++) {
    int x0, y0, w, h;
    buttonRect(b, x0, y0, w, h);
    if (x >= x0 && x < x0 + w && y >= y0 - 4 && y < y0 + h + 4) return b;
  }
  return B_NONE;
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
      pressedSince = lastRepeat = millis();
      if (pressedButton != B_NONE) bump();  // draw the highlight
    } else if ((pressedButton == B_VOL_DOWN || pressedButton == B_VOL_UP) && millis() - pressedSince > 500 &&
               millis() - lastRepeat > 150) {
      lastRepeat = millis();  // holding a volume button keeps stepping
      tap(pressedButton);
    }
  } else if (pressedButton != B_NONE) {
    uint8_t b = pressedButton;
    bool held = millis() - pressedSince > 500;
    pressedButton = B_NONE;
    bump();  // clear the highlight
    if (!(held && (b == B_VOL_DOWN || b == B_VOL_UP))) tap(b);
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

  lock = xSemaphoreCreateMutex();
  Serial.println("CYD-REMOTE READY");
  Serial.println("firmware: id=" FW_ID " flavor=" FW_FLAVOR " board=" FW_BOARD);
  startBle();
}

void loop() {
  static uint32_t drawnVersion = 0, drawnArt = UINT32_MAX, drawnSecond = UINT32_MAX;
  static Song shown;
  static bool firstDraw = true, noticeShown = false;
  static uint8_t shownPressed = B_NONE;
  static int shownUpdate = -1;

  int percent = updatePercent;
  bool done = updateDone;
  if (percent >= 0 || done) {
    if (done) percent = 100;
    if (percent != shownUpdate) {
      drawUpdate(percent, done, shownUpdate < 0);
      shownUpdate = percent;
    }
    if (done) {
      delay(1000);  // lets the "done" notification go out
      esp_restart();
    }
    delay(10);
    return;
  }
  if (shownUpdate >= 0) {  // an update stopped part-way: back to the usual screen
    shownUpdate = -1;
    firstDraw = true;
    drawnArt = UINT32_MAX;
  }

  pollTouch();

  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = version, av = artVersion;
  Song s = (v != drawnVersion) ? song : shown;
  uint8_t *art = nullptr;
  size_t artBytes = 0;
  bool clearArt = false;
  if (av != drawnArt) {
    art = artReady;
    artBytes = artReadyLen;
    artReady = nullptr;
    clearArt = artClear;
    artClear = false;
  }
  xSemaphoreGive(lock);

  if (art) {
    decodeArt(art, artBytes);
    free(art);
  } else if (clearArt) {
    artValid = false;
  }

  bool noticeOn = notice.length() && (int32_t)(noticeUntil - millis()) > 0;
  bool full = firstDraw || s.fromApp != nowPlayingLayout;
  if (full) {
    firstDraw = false;
    nowPlayingLayout = s.fromApp;
    pressedButton = B_NONE;  // the buttons have moved
    tft.fillScreen(BG);
  }

  if (!nowPlayingLayout) {
    if (full || v != drawnVersion || noticeOn != noticeShown) drawStatus(noticeOn);
    for (int b = 0; b < N_BUTTONS; b++)
      if (full || (pressedButton == b) != (shownPressed == b)) drawBigButton(b, pressedButton == b);
  } else {
    if (full || av != drawnArt) drawArt();
    if (full || s.title != shown.title || s.artist != shown.artist || s.album != shown.album ||
        s.source != shown.source || noticeOn != noticeShown || (!s.title.length() && s.playing != shown.playing))
      drawText(s, noticeOn ? notice : s.source);
    uint32_t pos = s.progressMs + (s.playing ? millis() - s.at : 0);
    if (s.durationMs) pos = min(pos, s.durationMs);
    if (full || pos / 1000 != drawnSecond || s.durationMs != shown.durationMs || s.volume != shown.volume) {
      drawnSecond = pos / 1000;
      drawProgress(pos, s.durationMs, s.volume);
    }
    for (int b = 0; b < N_BUTTONS; b++) {
      bool changed = full || (pressedButton == b) != (shownPressed == b);
      if (b == B_PLAY) changed |= s.playing != shown.playing;
      if (changed) drawSmallButton(b, s.playing, pressedButton == b);
    }
  }
  shown = s;
  shownPressed = pressedButton;
  noticeShown = noticeOn;
  drawnVersion = v;
  drawnArt = av;
  delay(10);
}
