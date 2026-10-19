// ESP32-2432S028 (CYD) as a Bluetooth now-playing screen. No Wi-Fi and no computer: a phone
// pairs with it like a Bluetooth speaker, and it shows what's playing (title, artist, album,
// progress, and the cover when the phone sends one) with touch buttons for previous,
// play/pause, next and volume. The sound comes out of the board's speaker socket, or of an
// I2S DAC board in the bluetooth-i2s build.
//
// Holding a finger on the cover or title for a second turns the sound here off (remembered
// across restarts), for a phone that plays to two devices at once (Samsung "Dual audio"):
// the CYD stays the phone's speaker for the song details and controls, and another speaker
// plays. The volume buttons then ask the phone to change its volume instead.
//
// Bluetooth roles (Classic Bluetooth, so phones see it as an ordinary speaker):
//   A2DP sink        receives the audio
//   AVRCP controller asks for the song details and cover, sends play/pause/next/previous
//   AVRCP target     absolute volume: the phone's volume slider and the buttons here stay in step
//
// Phones only send song details to the device they play to, which is why it is a speaker
// rather than a remote. Covers need AVRCP 1.6 on the phone (iPhone, recent Android; on Android
// it may need "Bluetooth AVRCP version: 1.6" in Developer options).
//
// Serial (SETUP_BAUD): a log, plus "FORGET" to unpair every phone. In the I2S build the log
// is silent if the DAC's data line uses TX (GPIO1), but FORGET still works.
//
// Bluetooth callbacks run in the Bluetooth task; the Arduino loop draws and reads the
// touchscreen. They share `player` under `lock`.

#include <Arduino.h>
#include <JPEGDEC.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <XPT2046_Touchscreen.h>

#if AUDIO_I2S
#include "driver/i2s_std.h"
#else
#include "driver/dac_continuous.h"
#endif
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "freertos/ringbuf.h"

#ifndef SETUP_BAUD
#define SETUP_BAUD 115200
#endif
#ifndef BT_NAME
#define BT_NAME "CYD Player"
#endif

// I2S DAC board (PCM5102 and similar). BCK and WS are on the CN1 connector (which also has
// 3.3 V and GND for the board), DOUT on TX of the P1 serial connector.
#if AUDIO_I2S
#ifndef I2S_BCK
#define I2S_BCK 27
#endif
#ifndef I2S_WS
#define I2S_WS 22
#endif
#ifndef I2S_DOUT
#define I2S_DOUT 1
#endif
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
static const size_t ART_MAX = 65536;  // AVRCP thumbnails are 200x200 JPEGs, usually under 20 KB

enum Button : uint8_t { B_PREV, B_PLAY, B_NEXT, B_VOL_DOWN, B_VOL_UP, N_BUTTONS, B_NONE = 255 };

static TFT_eSPI tft;
static JPEGDEC jpeg;
static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
static uint16_t BG, FG, DIM, TRACK, ACCENT;

// --- state shared between the Bluetooth task and the loop (guarded by `lock`) ---

struct Player {
  bool connected = false;  // a phone is connected for audio
  bool playing = false;
  String title, artist, album, device;
  uint32_t durationMs = 0, progressMs = 0, at = 0;  // progressMs as read at millis() == at
};

static SemaphoreHandle_t lock;
static Player player;
static uint8_t volume = 64;          // AVRCP absolute volume, 0..127
static String notice;                // shown briefly in place of the device name
static uint32_t noticeUntil = 0;
static uint32_t version = 1;         // bumped whenever the screen should be redrawn

// A cover arrives in pieces in the Bluetooth task and is decoded by the loop.
static uint8_t *artBuf = nullptr;    // being received (Bluetooth task only)
static size_t artLen = 0;
static uint8_t *artReady = nullptr;  // complete, waiting to be decoded (guarded by `lock`)
static size_t artReadyLen = 0;
static bool artClear = false;        // the song has no cover: show the placeholder
static uint32_t artVersion = 0;      // bumped when the loop should redraw the art area

static uint16_t artPixels[ART * ART];
static bool artValid = false;  // loop only

static void bump() { version++; }  // call with `lock` held

static void setNotice(const String &s) {
  xSemaphoreTake(lock, portMAX_DELAY);
  notice = s;
  noticeUntil = millis() + 3000;
  bump();
  xSemaphoreGive(lock);
  Serial.println(s);
}

// --- audio out ---
//
// A2DP -> ring buffer -> audio task -> either the DAC on GPIO26 (8-bit mono into the speaker
// socket's amplifier) or an I2S DAC board (16-bit stereo).

static RingbufHandle_t audioRing;
static uint32_t sampleRate = 44100;
static volatile bool rateChanged = false;
static volatile bool muted = false;  // the sound plays elsewhere (see the top of this file)
static Preferences prefs;

// Phones send audio at full scale once the speaker handles volume itself (absolute volume),
// so it is applied here. Squared, as loudness is heard roughly logarithmically.
static uint32_t gain() { return (uint32_t)volume * volume; }  // out of 127 * 127

#if AUDIO_I2S

static const size_t RING_BYTES = 32 * 1024;  // about 185 ms of 16-bit stereo
static i2s_chan_handle_t i2sTx;

// Bluetooth task: 16-bit stereo in and out, scaled by the volume, never blocking.
static void audioData(const uint8_t *data, uint32_t len) {
  if (muted) return;
  static int16_t out[512];
  const int16_t *pcm = (const int16_t *)data;
  uint32_t samples = len / 2, g = gain();
  while (samples) {
    uint32_t n = min<uint32_t>(samples, 512);
    for (uint32_t i = 0; i < n; i++) out[i] = (int32_t)pcm[i] * (int32_t)g / (127 * 127);
    xRingbufferSend(audioRing, out, n * 2, 0);  // full: drop rather than hold up Bluetooth
    pcm += n;
    samples -= n;
  }
}

static bool startAudio() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.auto_clear_after_cb = true;  // silence, not old buffers, when nothing is waiting
  i2s_std_config_t std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,  // PCM5102 boards make their own clock (SCK tied to GND)
          .bclk = (gpio_num_t)I2S_BCK,
          .ws = (gpio_num_t)I2S_WS,
          .dout = (gpio_num_t)I2S_DOUT,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {},
      },
  };
  std.clk_cfg.clk_src = I2S_CLK_SRC_APLL;  // the default clock can't hit 44.1 kHz closely
  if (i2s_new_channel(&chan, &i2sTx, nullptr) != ESP_OK) return false;
  if (i2s_channel_init_std_mode(i2sTx, &std) != ESP_OK || i2s_channel_enable(i2sTx) != ESP_OK) {
    i2s_del_channel(i2sTx);
    return false;
  }
  return true;
}

static void stopAudio() {
  i2s_channel_disable(i2sTx);
  i2s_del_channel(i2sTx);
}

static void writeAudio(const uint8_t *buf, size_t n) {
  if (buf) i2s_channel_write(i2sTx, buf, n, nullptr, portMAX_DELAY);
  else delay(5);  // the DMA plays silence by itself
}

#else

static const size_t RING_BYTES = 16 * 1024;  // about 370 ms of 8-bit mono
static dac_continuous_handle_t dac;

// Bluetooth task: 16-bit stereo in, 8-bit mono out, never blocking.
static void audioData(const uint8_t *data, uint32_t len) {
  if (muted) return;
  static uint8_t out[1024];
  const int16_t *pcm = (const int16_t *)data;
  uint32_t frames = len / 4, g = gain();
  while (frames) {
    uint32_t n = min<uint32_t>(frames, sizeof(out));
    for (uint32_t i = 0; i < n; i++) {
      int32_t s = ((int32_t)pcm[0] + pcm[1]) / 2;
      s = s * (int32_t)g / (127 * 127);
      out[i] = (uint8_t)((s >> 8) + 128);
      pcm += 2;
    }
    xRingbufferSend(audioRing, out, n, 0);  // full: drop rather than hold up Bluetooth
    frames -= n;
  }
}

static bool startAudio() {
  dac_continuous_config_t cfg = {
      .chan_mask = DAC_CHANNEL_MASK_CH1,  // GPIO26
      .desc_num = 4,
      .buf_size = 512,  // 4 x 512 samples queued: about 45 ms
      .freq_hz = sampleRate,
      .offset = 0,
      .clk_src = DAC_DIGI_CLK_SRC_APLL,  // the default clock can't hit 44.1 kHz closely
      .chan_mode = DAC_CHANNEL_MODE_SIMUL,
  };
  if (dac_continuous_new_channels(&cfg, &dac) != ESP_OK) return false;
  if (dac_continuous_enable(dac) != ESP_OK) {
    dac_continuous_del_channels(dac);
    return false;
  }
  return true;
}

static void stopAudio() {
  dac_continuous_disable(dac);
  dac_continuous_del_channels(dac);
}

// Silence (mid-scale) whenever no audio is waiting: the DMA would otherwise replay old
// buffers as a buzz. Writing blocks until the DAC has room, so the audio task paces itself.
static void writeAudio(const uint8_t *buf, size_t n) {
  static uint8_t silence[256];
  if (!buf) {
    memset(silence, 128, sizeof(silence));
    buf = silence;
    n = sizeof(silence);
  }
  dac_continuous_write(dac, (uint8_t *)buf, n, nullptr, -1);
}

#endif

static void audioTask(void *) {
  bool running = startAudio();
  if (!running) Serial.println("audio: output failed to start");
  for (;;) {
    if (rateChanged) {
      rateChanged = false;
      if (running) stopAudio();
      running = startAudio();
    }
    if (!running) {
      delay(100);
      continue;
    }
    size_t n = 0;
    uint8_t *buf = (uint8_t *)xRingbufferReceiveUpTo(audioRing, &n, 0, 1024);
    writeAudio(buf, n);
    if (buf) vRingbufferReturnItem(audioRing, buf);
  }
}

static void setMuted(bool m) {
  muted = m;
  prefs.putBool("muted", m);
  if (m) {  // empty what is already queued, so it stops at once
    size_t n;
    void *item;
    while ((item = xRingbufferReceiveUpTo(audioRing, &n, 0, RING_BYTES))) vRingbufferReturnItem(audioRing, item);
  }
}

// --- AVRCP ---

static esp_bd_addr_t peer;
static bool avrcConnected = false;
static bool coverArtConnected = false;
static bool volumeNotify = false;  // the phone asked to hear about volume changes
static uint8_t tlabel = 0;
static uint8_t coverHandle[ESP_AVRC_CA_IMAGE_HANDLE_LEN];  // of the cover on screen or on its way
static bool haveCoverHandle = false;

static uint8_t nextLabel() { return tlabel = (tlabel + 1) & 0x0F; }

static void requestMetadata() {
  uint8_t attrs = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM |
                  ESP_AVRC_MD_ATTR_PLAYING_TIME;
  if (coverArtConnected) attrs |= ESP_AVRC_MD_ATTR_COVER_ART;
  esp_avrc_ct_send_metadata_cmd(nextLabel(), attrs);
}

// Length, position and whether it's playing, in one reply.
static void requestPlayStatus() {
  if (avrcConnected) esp_avrc_ct_send_get_play_status_cmd(nextLabel());
}

static void registerNotification(esp_avrc_rn_event_ids_t event, uint32_t param = 0) {
  esp_avrc_ct_send_register_notification_cmd(nextLabel(), event, param);
}

static void sendKey(uint8_t key) {
  if (!avrcConnected) return;
  uint8_t tl = nextLabel();
  esp_avrc_ct_send_passthrough_cmd(tl, key, ESP_AVRC_PT_CMD_STATE_PRESSED);
  esp_avrc_ct_send_passthrough_cmd(tl, key, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

static void dropArt() {
  free(artBuf);
  artBuf = nullptr;
  artLen = 0;
}

// The cover is asked for on every song change; a song without one shows the placeholder.
static void showNoArt() {
  xSemaphoreTake(lock, portMAX_DELAY);
  free(artReady);
  artReady = nullptr;
  artClear = true;
  artVersion++;
  xSemaphoreGive(lock);
}

static void avrcCtEvent(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *p) {
  switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
      avrcConnected = p->conn_stat.connected;
      if (avrcConnected) {
        memcpy(peer, p->conn_stat.remote_bda, sizeof(peer));
        esp_avrc_ct_send_get_rn_capabilities_cmd(nextLabel());
      } else {
        coverArtConnected = false;
        haveCoverHandle = false;
        dropArt();
      }
      break;

    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
      if (p->rmt_feats.tg_feat_flag & ESP_AVRC_FEAT_FLAG_TG_COVER_ART) {
        esp_avrc_ct_cover_art_connect(ESP_AVRC_CA_MTU_MAX);
      } else {
        Serial.println("avrc: this phone doesn't send covers");
      }
      break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
      requestMetadata();
      requestPlayStatus();
      registerNotification(ESP_AVRC_RN_TRACK_CHANGE);
      registerNotification(ESP_AVRC_RN_PLAY_STATUS_CHANGE);
      registerNotification(ESP_AVRC_RN_PLAY_POS_CHANGED, 5);  // seconds between position updates
      break;

    case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT: {
      auto &r = p->play_status_rsp;
      uint8_t st = r.play_status;
      xSemaphoreTake(lock, portMAX_DELAY);
      if (r.song_length != 0xFFFFFFFF) player.durationMs = r.song_length;  // all ones: not known
      if (r.song_position != 0xFFFFFFFF) player.progressMs = r.song_position;
      player.at = millis();
      if (st != ESP_AVRC_PLAYBACK_ERROR) {
        player.playing = st == ESP_AVRC_PLAYBACK_PLAYING || st == ESP_AVRC_PLAYBACK_FWD_SEEK ||
                         st == ESP_AVRC_PLAYBACK_REV_SEEK;
      }
      bump();
      xSemaphoreGive(lock);
      break;
    }

    case ESP_AVRC_CT_METADATA_RSP_EVT: {
      String text = String((const char *)p->meta_rsp.attr_text).substring(0, p->meta_rsp.attr_length);
      if (p->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_COVER_ART) {
        // A 7-digit image handle; empty when the song has no cover.
        // Asked for again once cover art connects, so the same handle can arrive twice.
        if (p->meta_rsp.attr_length == ESP_AVRC_CA_IMAGE_HANDLE_LEN && coverArtConnected) {
          if (!haveCoverHandle || memcmp(coverHandle, p->meta_rsp.attr_text, sizeof(coverHandle))) {
            memcpy(coverHandle, p->meta_rsp.attr_text, sizeof(coverHandle));
            haveCoverHandle = true;
            dropArt();
            esp_avrc_ct_cover_art_get_linked_thumbnail(coverHandle);
          }
        } else {
          haveCoverHandle = false;
          showNoArt();
        }
        break;
      }
      xSemaphoreTake(lock, portMAX_DELAY);
      switch (p->meta_rsp.attr_id) {
        case ESP_AVRC_MD_ATTR_TITLE: player.title = text; break;
        case ESP_AVRC_MD_ATTR_ARTIST: player.artist = text; break;
        case ESP_AVRC_MD_ATTR_ALBUM: player.album = text; break;
        case ESP_AVRC_MD_ATTR_PLAYING_TIME: player.durationMs = text.toInt(); break;
      }
      bump();
      xSemaphoreGive(lock);
      break;
    }

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
      // Notifications fire once; each is registered again to hear about the next change.
      esp_avrc_rn_event_ids_t id = (esp_avrc_rn_event_ids_t)p->change_ntf.event_id;
      xSemaphoreTake(lock, portMAX_DELAY);
      switch (id) {
        case ESP_AVRC_RN_TRACK_CHANGE:
          player.progressMs = 0;
          player.at = millis();
          player.durationMs = 0;
          haveCoverHandle = false;
          bump();
          break;
        case ESP_AVRC_RN_PLAY_STATUS_CHANGE: {
          uint8_t st = p->change_ntf.event_parameter.playback;
          player.progressMs += player.playing ? millis() - player.at : 0;
          player.at = millis();
          player.playing = st == ESP_AVRC_PLAYBACK_PLAYING || st == ESP_AVRC_PLAYBACK_FWD_SEEK ||
                           st == ESP_AVRC_PLAYBACK_REV_SEEK;
          bump();
          break;
        }
        case ESP_AVRC_RN_PLAY_POS_CHANGED:
          player.progressMs = p->change_ntf.event_parameter.play_pos;
          player.at = millis();
          bump();
          break;
        default: break;
      }
      xSemaphoreGive(lock);
      if (id == ESP_AVRC_RN_TRACK_CHANGE) {
        // The placeholder until this song's cover arrives (a phone may send nothing for a
        // song without one, so the old cover can't just stay up).
        showNoArt();
        requestMetadata();
        requestPlayStatus();
        registerNotification(ESP_AVRC_RN_TRACK_CHANGE);
      } else if (id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
        registerNotification(ESP_AVRC_RN_PLAY_STATUS_CHANGE);
      } else if (id == ESP_AVRC_RN_PLAY_POS_CHANGED) {
        registerNotification(ESP_AVRC_RN_PLAY_POS_CHANGED, 5);
      }
      break;
    }

    case ESP_AVRC_CT_COVER_ART_STATE_EVT:
      coverArtConnected = p->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED;
      Serial.printf("avrc: cover art %s\n", coverArtConnected ? "connected" : "disconnected");
      if (coverArtConnected) requestMetadata();  // again, now including the cover handle
      break;

    case ESP_AVRC_CT_COVER_ART_DATA_EVT: {
      auto &d = p->cover_art_data;
      if (d.status != ESP_BT_STATUS_SUCCESS) {
        dropArt();
        showNoArt();
        break;
      }
      if (d.data_len) {
        if (artLen + d.data_len > ART_MAX) {
          dropArt();
          artLen = ART_MAX + 1;  // too big: ignore the rest
        } else if (artLen <= ART_MAX) {
          if (!artBuf) artBuf = (uint8_t *)malloc(ART_MAX);
          if (artBuf) {
            memcpy(artBuf + artLen, d.p_data, d.data_len);
            artLen += d.data_len;
          }
        }
      }
      if (d.final) {
        if (artBuf && artLen <= ART_MAX) {
          xSemaphoreTake(lock, portMAX_DELAY);
          free(artReady);
          artReady = artBuf;
          artReadyLen = artLen;
          artVersion++;
          xSemaphoreGive(lock);
          artBuf = nullptr;
          artLen = 0;
        } else {
          dropArt();
          showNoArt();
        }
      }
      break;
    }

    default: break;
  }
}

static void sendVolumeChanged() {
  if (!volumeNotify) return;
  volumeNotify = false;  // the phone registers again after each change
  esp_avrc_rn_param_t rn = {};
  rn.volume = volume;
  esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
}

static void avrcTgEvent(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *p) {
  switch (event) {
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
      xSemaphoreTake(lock, portMAX_DELAY);
      volume = p->set_abs_vol.volume & 0x7F;
      bump();
      xSemaphoreGive(lock);
      break;
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
      if (p->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        volumeNotify = true;
        esp_avrc_rn_param_t rn = {};
        rn.volume = volume;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
      }
      break;
    default: break;
  }
}

// Volume buttons: change it here and tell the phone, whose slider then follows. With the
// sound off here they ask the phone to change its own volume (some phones ignore this).
static void stepVolume(int delta) {
  if (muted) {
    sendKey(delta > 0 ? ESP_AVRC_PT_CMD_VOL_UP : ESP_AVRC_PT_CMD_VOL_DOWN);
    return;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  volume = constrain((int)volume + delta, 0, 127);
  bump();
  xSemaphoreGive(lock);
  sendVolumeChanged();
}

// --- A2DP and pairing ---

// Arduino frees the Bluetooth stack's memory at boot unless the sketch says it uses it.
bool btInUse() { return true; }

static void a2dEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *p) {
  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
      bool on = p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
      bool off = p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED;
      if (!on && !off) break;
      // Hidden while a phone is connected, so a second one can't pair in the middle of a song.
      esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, on ? ESP_BT_NON_DISCOVERABLE : ESP_BT_GENERAL_DISCOVERABLE);
      xSemaphoreTake(lock, portMAX_DELAY);
      if (off) player = Player();  // on connect, keep any song details that arrived first
      player.connected = on;
      bump();
      xSemaphoreGive(lock);
      if (off) showNoArt();
      if (on) esp_bt_gap_read_remote_name(p->conn_stat.remote_bda);
      Serial.println(on ? "a2dp: connected" : "a2dp: disconnected");
      break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {
      uint8_t f = p->audio_cfg.mcc.cie.sbc_info.samp_freq;  // SBC, the only codec offered
      uint32_t rate = f & 0x8 ? 16000 : f & 0x4 ? 32000 : f & 0x2 ? 44100 : 48000;
      if (rate != sampleRate) {
        sampleRate = rate;
        rateChanged = true;
      }
      Serial.printf("a2dp: %u Hz\n", rate);
      break;
    }
    default: break;
  }
}

static void gapEvent(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p) {
  switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        setNotice(String("Paired with ") + (const char *)p->auth_cmpl.device_name);
      } else {
        setNotice("Pairing failed");
      }
      break;
    case ESP_BT_GAP_CFM_REQ_EVT:  // numeric comparison: there's no keyboard here, so accept
      esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
      break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
      if (p->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
        xSemaphoreTake(lock, portMAX_DELAY);
        player.device = (const char *)p->read_rmt_name.rmt_name;
        bump();
        xSemaphoreGive(lock);
      }
      break;
    default: break;
  }
}

// On power-up, try the most recently paired phone so it reconnects without opening its
// Bluetooth settings (it still has to be in range with Bluetooth on).
static void reconnectLastPhone() {
  int n = esp_bt_gap_get_bond_device_num();
  if (n <= 0) return;
  esp_bd_addr_t *list = (esp_bd_addr_t *)malloc(n * sizeof(esp_bd_addr_t));
  if (!list) return;
  if (esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK && n > 0) {
    esp_a2d_sink_connect(list[n - 1]);
  }
  free(list);
}

static bool btFailed(const char *step, esp_err_t err) {
  Serial.printf("bt: %s failed: %s\n", step, esp_err_to_name(err));
  return false;
}

static bool startBluetooth() {
  // Classic Bluetooth only: the memory BLE would take is handed back.
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  cfg.mode = ESP_BT_MODE_CLASSIC_BT;  // the default is both, and it must match the enable below
  esp_err_t err;
  if ((err = esp_bt_controller_init(&cfg)) != ESP_OK) return btFailed("controller init", err);
  if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) return btFailed("controller enable", err);
  if ((err = esp_bluedroid_init()) != ESP_OK) return btFailed("bluedroid init", err);
  if ((err = esp_bluedroid_enable()) != ESP_OK) return btFailed("bluedroid enable", err);
  esp_bt_gap_set_device_name(BT_NAME);
  esp_bt_gap_register_callback(gapEvent);

  esp_bt_sp_param_t param = ESP_BT_SP_IOCAP_MODE;
  esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;  // "just works" pairing, no code to type
  esp_bt_gap_set_security_param(param, &iocap, sizeof(iocap));

  esp_avrc_ct_init();
  esp_avrc_ct_register_callback(avrcCtEvent);
  esp_avrc_tg_init();
  esp_avrc_tg_register_callback(avrcTgEvent);
  esp_avrc_rn_evt_cap_mask_t caps = {};
  esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &caps, ESP_AVRC_RN_VOLUME_CHANGE);
  esp_avrc_tg_set_rn_evt_cap(&caps);

  esp_a2d_register_callback(a2dEvent);
  esp_a2d_sink_register_data_callback(audioData);
  esp_a2d_sink_init();

  // Shows up as a loudspeaker in the phone's Bluetooth list.
  esp_bt_cod_t cod = {};
  cod.major = ESP_BT_COD_MAJOR_DEV_AV;
  cod.minor = 0x05;  // loudspeaker
  esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);

  err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
  const uint8_t *a = esp_bt_dev_get_address();
  Serial.printf("bt: \"%s\" %02X:%02X:%02X:%02X:%02X:%02X, %s\n", BT_NAME, a[0], a[1], a[2], a[3], a[4], a[5],
                err == ESP_OK ? "visible for pairing" : esp_err_to_name(err));
  reconnectLastPhone();
  return true;
}

static void forgetPhones() {
  int n = esp_bt_gap_get_bond_device_num();
  if (n > 0) {
    esp_bd_addr_t *list = (esp_bd_addr_t *)malloc(n * sizeof(esp_bd_addr_t));
    if (list && esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK) {
      for (int i = 0; i < n; i++) esp_bt_gap_remove_bond_device(list[i]);
    }
    free(list);
  }
  Serial.printf("OK, forgot %d phone(s)\n", max(n, 0));
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

static void drawWaiting(const String &note) {
  tft.fillScreen(BG);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(FG);
  tft.drawString("Pair your phone with", W / 2, H / 2 - 30);
  tft.setTextColor(ACCENT);
  tft.drawString(BT_NAME, W / 2, H / 2 - 4);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(DIM);
  tft.drawString("in its Bluetooth settings", W / 2, H / 2 + 22);
  if (note.length()) tft.drawString(fitText(toAscii(note), W - 24), W / 2, H - 24);
  tft.setTextDatum(TL_DATUM);
}

static void drawText(const Player &p, const String &line) {
  tft.fillRect(TEXT_X, ART_Y, W - TEXT_X, ART, BG);
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  String title = p.title.length() ? p.title : String(p.playing ? "Playing" : "Nothing playing");
  int y = drawWrapped(toAscii(title), TEXT_X, ART_Y + 2, TEXT_W, 3, 22, FG) + 4;
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

// The progress bar, with the volume (or "Sound off") in the middle of the line below it.
static void drawProgress(uint32_t pos, uint32_t len, uint8_t vol, bool mute) {
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
  tft.setTextDatum(TC_DATUM);
  tft.drawString(mute ? String("Sound off") : String("Volume ") + (vol * 100 + 63) / 127 + "%", W / 2, BAR_Y + 9);
  tft.setTextDatum(TL_DATUM);
}

static void drawSpeaker(int cx, int cy, uint16_t c) {
  tft.fillRect(cx - 14, cy - 5, 6, 11, c);
  tft.fillTriangle(cx - 9, cy, cx - 1, cy - 10, cx - 1, cy + 10, c);
}

static void drawButton(int b, const Player &p, bool pressed) {
  int bw = W / N_BUTTONS, x0 = b * bw, cx = x0 + bw / 2, cy = (BTN_Y0 + BTN_Y1) / 2;
  tft.fillRect(x0, BTN_Y0 - 2, bw, H - BTN_Y0 + 2, BG);
  if (pressed) tft.fillRoundRect(x0 + 6, BTN_Y0, bw - 12, BTN_Y1 - BTN_Y0, 10, TRACK);
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
    case B_VOL_DOWN:
    case B_VOL_UP:
      drawSpeaker(cx, cy, FG);
      tft.fillRect(cx + 3, cy - 1, 11, 3, FG);  // minus
      if (b == B_VOL_UP) tft.fillRect(cx + 7, cy - 5, 3, 11, FG);  // plus
      break;
  }
}

// --- touch ---

static uint8_t pressedButton = B_NONE;
static uint32_t pressedSince = 0, lastRepeat = 0;
static uint32_t holdSince = 0;  // finger on the cover or title, for the sound on/off hold
static bool holdDone = false;   // toggled already during this touch

static uint8_t buttonAt(int x, int y) {
  if (y < BTN_Y0 - 8) return B_NONE;
  return constrain(x * N_BUTTONS / W, 0, N_BUTTONS - 1);
}

static const int VOL_STEP = 8;  // 16 steps from silent to full, like most phones

// Shows the result of a tap straight away, then tells the phone.
static void press(uint8_t b) {
  bool playing;
  xSemaphoreTake(lock, portMAX_DELAY);
  Player &p = player;
  if (b == B_PLAY) {
    uint32_t now = millis();
    p.progressMs += p.playing ? now - p.at : 0;
    p.at = now;
    p.playing = !p.playing;
    bump();
  }
  playing = p.playing;
  xSemaphoreGive(lock);
  switch (b) {
    case B_PLAY: sendKey(playing ? ESP_AVRC_PT_CMD_PLAY : ESP_AVRC_PT_CMD_PAUSE); break;
    case B_PREV: sendKey(ESP_AVRC_PT_CMD_BACKWARD); break;
    case B_NEXT: sendKey(ESP_AVRC_PT_CMD_FORWARD); break;
    case B_VOL_DOWN: stepVolume(-VOL_STEP); break;
    case B_VOL_UP: stepVolume(VOL_STEP); break;
  }
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
    if (pressedButton == B_NONE && buttonAt(x, y) == B_NONE) {
      if (!holdSince) holdSince = millis();
      if (!holdDone && millis() - holdSince > 1000) {
        holdDone = true;
        setMuted(!muted);
        setNotice(muted ? "Sound off here" : "Sound on");
      }
    } else if (pressedButton == B_NONE) {
      pressedButton = buttonAt(x, y);
      pressedSince = lastRepeat = millis();
      if (pressedButton != B_NONE) {
        xSemaphoreTake(lock, portMAX_DELAY);
        bump();  // draw the highlight
        xSemaphoreGive(lock);
      }
    } else if ((pressedButton == B_VOL_DOWN || pressedButton == B_VOL_UP) && millis() - pressedSince > 500 &&
               millis() - lastRepeat > 150) {
      lastRepeat = millis();  // holding a volume button keeps stepping
      stepVolume(pressedButton == B_VOL_UP ? VOL_STEP / 2 : -VOL_STEP / 2);
    }
  } else if (holdSince) {
    holdSince = 0;
    holdDone = false;
  }
  if (!down && pressedButton != B_NONE) {
    uint8_t b = pressedButton;
    bool held = millis() - pressedSince > 500;
    pressedButton = B_NONE;
    if (!(held && (b == B_VOL_DOWN || b == B_VOL_UP))) press(b);
    else {
      xSemaphoreTake(lock, portMAX_DELAY);
      bump();  // clear the highlight
      xSemaphoreGive(lock);
    }
  }
}

// --- serial ---

static void readSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line.trim();
      if (line == "FORGET") forgetPhones();
      else if (line.length()) Serial.println("ERR unknown command (try FORGET)");
      line = "";
    } else if (line.length() < 64) {
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

  lock = xSemaphoreCreateMutex();
  prefs.begin("cyd-bluetooth");
  muted = prefs.getBool("muted", false);
  audioRing = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_BYTEBUF);
  xTaskCreatePinnedToCore(audioTask, "audio", 3072, nullptr, 5, nullptr, 1);
  Serial.println("CYD-BLUETOOTH READY");
  if (!startBluetooth()) {
    xSemaphoreTake(lock, portMAX_DELAY);
    notice = "Bluetooth failed to start";
    noticeUntil = UINT32_MAX;
    xSemaphoreGive(lock);
  }
}

void loop() {
  static uint32_t drawnVersion = 0, drawnArt = UINT32_MAX, drawnSecond = UINT32_MAX;
  static Player shown;
  static bool shownConnected = false, noticeShown = false;
  static uint8_t shownPressed = B_NONE, shownVolume = 255;
  static bool shownMuted = false;

  readSerial();
  pollTouch();

  static uint32_t lastStatus = 0;
  if (millis() - lastStatus > 5000) {  // keeps the progress bar right on phones that don't report it
    lastStatus = millis();
    requestPlayStatus();
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = version, av = artVersion;
  Player p = (v != drawnVersion) ? player : shown;
  uint8_t vol = volume;
  String note = notice;
  bool noticeOn = note.length() && (int32_t)(noticeUntil - millis()) > 0;
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

  if (!p.connected) {
    if (v != drawnVersion || shownConnected || noticeOn != noticeShown) drawWaiting(noticeOn ? note : "");
    shown = p;
    shownConnected = false;
    noticeShown = noticeOn;
    drawnVersion = v;
    drawnArt = av;
    delay(10);
    return;
  }

  bool full = !shownConnected;
  if (full) tft.fillScreen(BG);
  if (full || av != drawnArt) {
    drawnArt = av;
    drawArt();
  }
  if (full || p.title != shown.title || p.artist != shown.artist || p.album != shown.album ||
      p.device != shown.device || noticeOn != noticeShown || (!p.title.length() && p.playing != shown.playing)) {
    drawText(p, noticeOn ? note : p.device);
    noticeShown = noticeOn;
  }
  uint32_t pos = p.progressMs + (p.playing ? millis() - p.at : 0);
  if (p.durationMs) pos = min(pos, p.durationMs);
  bool mute = muted;
  if (full || pos / 1000 != drawnSecond || p.durationMs != shown.durationMs || vol != shownVolume ||
      mute != shownMuted) {
    drawnSecond = pos / 1000;
    shownVolume = vol;
    shownMuted = mute;
    drawProgress(pos, p.durationMs, vol, mute);
  }
  for (int b = 0; b < N_BUTTONS; b++) {
    bool changed = full || (pressedButton == b) != (shownPressed == b);
    if (b == B_PLAY) changed |= p.playing != shown.playing;
    if (changed) drawButton(b, p, pressedButton == b);
  }
  shown = p;
  shownConnected = true;
  shownPressed = pressedButton;
  drawnVersion = v;
  delay(10);
}
