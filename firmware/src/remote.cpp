// ESP32-2432S028 (CYD) as a Bluetooth media remote. It pairs as a Bluetooth LE media-key
// device (HID consumer control, like the media keys on a keyboard), so the phone or computer
// keeps playing through its own speaker or headphones. Buttons: previous, play/pause, next,
// volume down and up. A remote can't receive song details, so there is no title or cover.
//
// Serial (SETUP_BAUD): a log.

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include <atomic>

#ifndef SETUP_BAUD
#define SETUP_BAUD 115200
#endif
#ifndef BT_NAME
#define BT_NAME "CYD Remote"
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
static const int TOP_H = 44;                  // status bar
static const int ROW1_Y0 = 52, ROW1_Y1 = 140;  // previous, play/pause, next
static const int ROW2_Y0 = 148, ROW2_Y1 = 232; // volume down, volume up

enum Button : uint8_t { B_PREV, B_PLAY, B_NEXT, B_VOL_DOWN, B_VOL_UP, N_BUTTONS, B_NONE = 255 };

// HID consumer-page usages (USB HID Usage Tables, section 15).
static const uint16_t USAGE[N_BUTTONS] = {0xB6, 0xCD, 0xB5, 0xEA, 0xE9};

static TFT_eSPI tft;
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

static BLEHIDDevice *hid;
static BLECharacteristic *input;
static volatile bool connected = false;
static std::atomic<uint32_t> version{1};  // bumped whenever the status bar should be redrawn
static String notice;                  // shown briefly in the status bar (loop only)
static uint32_t noticeUntil = 0;

class ServerEvents : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    connected = true;
    version++;
    Serial.println("ble: connected");
  }
  void onDisconnect(BLEServer *) override {
    connected = false;
    version++;
    Serial.println("ble: disconnected");
    BLEDevice::startAdvertising();  // visible again for this or another device
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
    version++;
    return;
  }
  sendUsage(USAGE[b]);
  delay(10);
  sendUsage(0);
}

// --- drawing ---

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

static void drawStatus() {
  tft.fillRect(0, 0, W, TOP_H, BG);
  tft.setTextDatum(ML_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  bool noticeOn = notice.length() && (int32_t)(noticeUntil - millis()) > 0;
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

static void drawSpeaker(int cx, int cy, uint16_t c) {
  tft.fillRect(cx - 22, cy - 8, 10, 17, c);
  tft.fillTriangle(cx - 14, cy, cx - 1, cy - 16, cx - 1, cy + 16, c);
}

static void drawButton(int b, bool pressed) {
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
    case B_PLAY:  // a remote can't tell which, so both
      tft.fillTriangle(cx - 22, cy - 16, cx - 22, cy + 16, cx + 2, cy, c);
      tft.fillRect(cx + 8, cy - 16, 7, 33, c);
      tft.fillRect(cx + 20, cy - 16, 7, 33, c);
      break;
    case B_VOL_DOWN:
    case B_VOL_UP:
      drawSpeaker(cx, cy, c);
      tft.fillRect(cx + 6, cy - 2, 18, 5, c);  // minus
      if (b == B_VOL_UP) tft.fillRect(cx + 13, cy - 9, 5, 19, c);  // plus
      break;
  }
}

// --- touch ---

static uint8_t pressedButton = B_NONE;
static uint32_t pressedSince = 0, lastRepeat = 0;

static uint8_t buttonAt(int x, int y) {
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
      if (pressedButton != B_NONE) drawButton(pressedButton, true);
    } else if ((pressedButton == B_VOL_DOWN || pressedButton == B_VOL_UP) && millis() - pressedSince > 500 &&
               millis() - lastRepeat > 150) {
      lastRepeat = millis();  // holding a volume button keeps stepping
      tap(pressedButton);
    }
  } else if (pressedButton != B_NONE) {
    uint8_t b = pressedButton;
    bool held = millis() - pressedSince > 500;
    pressedButton = B_NONE;
    drawButton(b, false);
    if (!(held && (b == B_VOL_DOWN || b == B_VOL_UP))) tap(b);
  }
}

// --- main ---

void setup() {
  Serial.begin(SETUP_BAUD);
  tft.init();
  tft.setRotation(1);
  BG = tft.color565(12, 14, 20);
  FG = tft.color565(235, 238, 245);
  DIM = tft.color565(140, 146, 160);
  TRACK = tft.color565(40, 44, 56);
  ACCENT = tft.color565(30, 215, 96);
  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(1);

  tft.fillScreen(BG);
  for (int b = 0; b < N_BUTTONS; b++) drawButton(b, false);
  Serial.println("CYD-REMOTE READY");
  startBle();
}

void loop() {
  static uint32_t drawnVersion = 0;
  static bool noticeShown = false;
  pollTouch();
  bool noticeOn = notice.length() && (int32_t)(noticeUntil - millis()) > 0;
  if (version != drawnVersion || noticeOn != noticeShown) {
    drawnVersion = version;
    noticeShown = noticeOn;
    drawStatus();
  }
  delay(10);
}
