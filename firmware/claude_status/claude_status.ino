/*
 * Claude Code Display — ESP32 + 0.96" OLED status indicator
 *
 * A small WiFi-connected display that shows the current state of one or more
 * Claude Code sessions, so you don't have to keep your eyes glued to the
 * terminal. The screen flashes when Claude needs your permission.
 *
 * Features:
 *   - Tracks up to 4 concurrent sessions, distinguished by session_id
 *   - Display priority: WAITING > ERROR > WORKING > recent DONE
 *     Same-priority sessions rotate every 5 s
 *   - NTP-synced clock when idle
 *   - Today's prompt count + working minutes, persisted to NVS
 *   - WORKING auto-reverts to IDLE after 60 s of no updates
 *     (handles user interrupts / missed Stop hooks)
 *   - UTF-8 text rendering (Chinese, etc.) via u8g2 + GB2312 font
 *
 * HTTP endpoints:
 *   POST /status   push a state update   JSON: {state,msg,project,session}
 *   GET  /status   list all active sessions
 *   POST /clear    drop all active sessions
 *   GET  /         simple HTML status page
 *
 * Wiring (ESP32-C3 Super Mini, change pins below for other boards):
 *   OLED GND -> GND   VCC -> 3V3   SCL -> GPIO9   SDA -> GPIO8
 *
 * Required libraries (install via Arduino Library Manager):
 *   - U8g2          by Oliver
 *   - ArduinoJson   by Benoit Blanchon
 *   (WiFi / WebServer / ESPmDNS / Preferences / time are bundled with ESP32 core)
 *
 * Setup:
 *   1. Copy `config.h.example` to `config.h` and fill in your WiFi.
 *   2. Select board "ESP32C3 Dev Module" (or your variant).
 *   3. Compile + upload.
 */

// Compile-time defaults — everything user-facing (WiFi creds + timezone) is
// configured at runtime via the captive-portal web page and stored in NVS.
// `config.h` is no longer required; if one exists from an older checkout it
// can still override these defaults at build time.
#if __has_include("config.h")
  #include "config.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID      ""
#endif
#ifndef WIFI_PASSWORD
  #define WIFI_PASSWORD  ""
#endif
#ifndef MDNS_NAME
  #define MDNS_NAME      "claude-display"
#endif
#ifndef TZ_OFFSET_SEC
  #define TZ_OFFSET_SEC  0
#endif

#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DNSServer.h>

// FluxGarage RoboEyes is an Adafruit-GFX-style template. We don't pull in
// Adafruit_GFX/SSD1306 (would conflict with u8g2 + bloat flash) — instead a
// tiny adapter below maps the few methods RoboEyes calls onto u8g2 primitives.
// `DEFAULT` is defined as 0 by RoboEyes (mood enum); some ESP32 headers also
// define it. We don't use the mood-DEFAULT macro outside the header.
#include "FluxGarage_RoboEyes.h"

// ============== Hardware pins / OLED ==============
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define SCREEN_ADDRESS 0x3C       // I2C address of SSD1306
#define I2C_SDA        8          // ESP32-C3 Super Mini: GPIO8
#define I2C_SCL        9          // ESP32-C3 Super Mini: GPIO9

// ============== NTP ==============
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.google.com";

// ============== Behavior tuning ==============
const int      MAX_SESSIONS         = 4;
const uint32_t SESSION_STALE_MS     = 5UL * 60UL * 1000UL;   // 5 min idle -> drop
const uint32_t SESSION_ROTATE_MS    = 5000;                   // multi-session rotation
const uint32_t WORKING_TIMEOUT_MS   = 300000;                 // WORKING auto-revert -> IDLE (5 min — long builds, npm/cargo, etc.)
const uint32_t DONE_LINGER_MS       = 5000;                   // DONE shows briefly then yields
const uint32_t DRAW_INTERVAL_MS     = 40;                     // idle screen needs ~25 fps for smooth RoboEyes; status screens look fine too
const uint32_t BTC_REFRESH_MS       = 60000;                  // CoinGecko free tier is fine at 1/min
const uint32_t BTC_STALE_MS         = 15UL * 60UL * 1000UL;   // hide price after 15 min of no updates

// ============== State enum ==============
// Must precede every function — the Arduino IDE inserts auto-generated
// prototypes before the first function, so any type used in a signature has
// to be declared before that point.
enum State { ST_IDLE, ST_WORKING, ST_WAITING, ST_DONE, ST_ERROR };

// ============== Globals ==============
// u8g2 full-buffer, hardware I2C using the pins set on Wire above
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
WebServer        server(80);
DNSServer        dnsServer;     // captive-portal DNS — only active while inApMode
Preferences      prefs;
bool             inApMode = false;

// ============== RoboEyes <-> u8g2 adapter ==============
// FluxGarage_RoboEyes.h is templated on an Adafruit-GFX-style display class
// and only uses four methods: clearDisplay / display / fillRoundRect /
// fillTriangle. We forward the two drawing methods to u8g2 and no-op the
// buffer flush methods — the idle screen owns clearBuffer()/sendBuffer() so
// eyes can be composited under the time / BTC text.
//
// `yOff` shifts all RoboEyes drawing down by a fixed offset so the eyes
// inhabit a middle band of the screen and leave room for text top + bottom.
struct U8g2RoboAdapter {
  U8G2* g;
  int   yOff;

  U8g2RoboAdapter(U8G2& gg, int yOffset) : g(&gg), yOff(yOffset) {}

  void clearDisplay() {}
  void display()     {}

  void fillRoundRect(int x, int y, int w, int h, int r, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    g->setDrawColor(color);
    int maxR = ((w < h ? w : h) - 1) / 2;
    if (r > maxR) r = maxR;
    if (r < 1 || w < 3 || h < 3) g->drawBox(x, y + yOff, w, h);
    else                          g->drawRBox(x, y + yOff, w, h, r);
  }

  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color) {
    g->setDrawColor(color);
    g->drawTriangle(x0, y0 + yOff, x1, y1 + yOff, x2, y2 + yOff);
  }
};

// Idle "clock + eyes" page: top text strip 0..12, eye band fills the rest.
// No bottom text — keeps the eyes uncluttered (much nicer to look at).
const int ROBO_YOFF        = 14;
const int ROBO_BAND_HEIGHT = 50;

// How long each idle sub-page is visible before rotating.
const uint32_t IDLE_EYES_MS   = 12000;   // clock + eyes — give them the spotlight
const uint32_t IDLE_CRYPTO_MS = 6000;    // crypto page — quick glance

U8g2RoboAdapter         roboAdapter(display, ROBO_YOFF);
RoboEyes<U8g2RoboAdapter> roboEyes(roboAdapter);

// ============== Crypto prices (background fetcher) ==============
// CoinGecko's simple-price endpoint is HTTPS-only. We accept any cert
// (setInsecure) to keep flash usage down — we're displaying public numbers
// and don't authenticate, so MITM "tampering" only changes the displayed
// price, no security boundary.
struct Coin {
  const char*     sym;       // shown on screen, also Binance symbol prefix (BTC -> BTCUSDT)
  volatile double usd;       // last known USD price (0 = unknown)
};

Coin coins[] = {
  { "BTC", 0 },
  { "ETH", 0 },
  { "SOL", 0 },
};
const int N_COINS = sizeof(coins) / sizeof(coins[0]);

volatile uint32_t coinsUpdatedAt = 0;
volatile uint32_t coinsLastTryAt = 0;
volatile bool     coinsLastOk    = false;

// Font shorthands — fonts live in flash.
// wqy12 GB2312 covers ~6700 Chinese chars + ASCII; needed for Chinese messages.
#define FONT_CN_12   u8g2_font_wqy12_t_gb2312    // 12 px Chinese (body)        ~80 KB
#define FONT_LRG     u8g2_font_helvB14_tr        // 14 px bold ASCII (titles)   ~3 KB
#define FONT_BIG_NUM u8g2_font_logisoso24_tn     // 24 px digits (clock)        ~5 KB
#define FONT_TINY    u8g2_font_5x8_tf            // 5x8 ASCII (footer text)     ~1 KB

// u8g2 uses 0/1 for draw color; aliases for readability.
#define COLOR_FG 1
#define COLOR_BG 0

// Adafruit-GFX-style cursor (top-left). u8g2 draws at the baseline, so we add
// the font's ascent to convert.
inline void drawText(int x, int yTop, const char* s) {
  display.drawUTF8(x, yTop + display.getAscent(), s);
}
inline void drawText(int x, int yTop, const String& s) {
  drawText(x, yTop, s.c_str());
}

const char* stateName(State s) {
  switch (s) {
    case ST_IDLE:    return "IDLE";
    case ST_WORKING: return "WORKING";
    case ST_WAITING: return "WAITING";
    case ST_DONE:    return "DONE";
    case ST_ERROR:   return "ERROR";
  }
  return "?";
}

State parseState(const String& s) {
  if (s == "working") return ST_WORKING;
  if (s == "waiting") return ST_WAITING;
  if (s == "done")    return ST_DONE;
  if (s == "error")   return ST_ERROR;
  return ST_IDLE;
}

// ============== Session table ==============
struct Session {
  String   id;
  String   project;
  State    state;
  String   msg;
  uint32_t updatedAt;
  uint32_t workingStartedAt;
  uint32_t lastDurationMs;
  uint32_t promptCount;
  bool     active;
};

Session sessions[MAX_SESSIONS];

// ============== NTP helpers ==============
bool getCurrentLocalTime(struct tm* outTm) {
  if (!getLocalTime(outTm, 50)) return false;
  if (outTm->tm_year < 120) return false;  // not synced yet (returns 1970)
  return true;
}

int currentDateInt() {
  struct tm tm;
  if (!getCurrentLocalTime(&tm)) return 0;
  return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

// ============== Persistent daily stats (NVS) ==============
struct DailyStats {
  uint32_t prompts;     // prompts today
  uint32_t workingMs;   // total working time today (ms)
  int      day;         // YYYYMMDD
};

DailyStats stats = {0, 0, 0};
uint32_t   lastStatsSave = 0;

void statsLoad() {
  stats.day       = prefs.getInt ("day",     0);
  stats.prompts   = prefs.getUInt("prompts", 0);
  stats.workingMs = prefs.getUInt("workms",  0);
}

void statsSave() {
  prefs.putInt ("day",     stats.day);
  prefs.putUInt("prompts", stats.prompts);
  prefs.putUInt("workms",  stats.workingMs);
  lastStatsSave = millis();
}

// Avoid hammering NVS — flash has limited write cycles.
void statsSaveThrottled() {
  if (millis() - lastStatsSave > 5000) statsSave();
}

void statsCheckRollover() {
  int curDay = currentDateInt();
  if (curDay > 0 && curDay != stats.day) {
    stats.day       = curDay;
    stats.prompts   = 0;
    stats.workingMs = 0;
    statsSave();
  }
}

// ============== Session management ==============
int findSession(const String& id) {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].active && sessions[i].id == id) return i;
  }
  return -1;
}

void initSession(int idx, const String& id) {
  sessions[idx].id               = id;
  sessions[idx].project          = "";
  sessions[idx].state            = ST_IDLE;
  sessions[idx].msg              = "";
  sessions[idx].updatedAt        = millis();
  sessions[idx].workingStartedAt = 0;
  sessions[idx].lastDurationMs   = 0;
  sessions[idx].promptCount      = 0;
  sessions[idx].active           = true;
}

int findOrAllocSession(const String& id) {
  int idx = findSession(id);
  if (idx >= 0) return idx;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) { initSession(i, id); return i; }
  }
  // All slots taken: replace the least-recently-updated one.
  int oldest = 0;
  for (int i = 1; i < MAX_SESSIONS; i++) {
    if (sessions[i].updatedAt < sessions[oldest].updatedAt) oldest = i;
  }
  initSession(oldest, id);
  return oldest;
}

void evictStaleSessions() {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) continue;
    if ((millis() - sessions[i].updatedAt) > SESSION_STALE_MS) {
      sessions[i].active = false;
    }
  }
}

int activeSessionCount() {
  int n = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].active) n++;
  return n;
}

// Priority classes: WAITING > ERROR > WORKING > recently-DONE.
// Sessions in the same class are rotated by time. Different classes never
// fight for screen — DONE sessions don't compete with active WORKING.
// Returns -1 when nothing is worth showing (so we draw the idle/clock screen).
int pickDisplaySession(int* outOrdinal, int* outTotal) {
  int waiting[MAX_SESSIONS]; int nW = 0;
  int errors[MAX_SESSIONS];  int nE = 0;
  int working[MAX_SESSIONS]; int nWk = 0;
  int recent[MAX_SESSIONS];  int nR = 0;

  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) continue;
    switch (sessions[i].state) {
      case ST_WAITING: waiting[nW++] = i; break;
      case ST_ERROR:   errors[nE++]  = i; break;
      case ST_WORKING: working[nWk++] = i; break;
      case ST_DONE:
        if ((millis() - sessions[i].updatedAt) <= DONE_LINGER_MS) {
          recent[nR++] = i;
        }
        break;
      default: break;  // IDLE: never displayed (clock takes over)
    }
  }

  int* list = nullptr; int n = 0;
  if      (nW  > 0) { list = waiting; n = nW;  }
  else if (nE  > 0) { list = errors;  n = nE;  }
  else if (nWk > 0) { list = working; n = nWk; }
  else if (nR  > 0) { list = recent;  n = nR;  }

  if (outTotal) *outTotal = n;
  if (n == 0) {
    if (outOrdinal) *outOrdinal = 0;
    return -1;
  }

  uint32_t bucket = (millis() / SESSION_ROTATE_MS) % n;
  if (outOrdinal) *outOrdinal = bucket + 1;
  return list[bucket];
}

// ============== Status icon (16x16) ==============
// u8g2 paints with the current draw color — set it before calling.
void drawIcon(State s, int x, int y) {
  display.drawFrame(x, y, 16, 16);
  switch (s) {
    case ST_IDLE:
      display.drawCircle(x + 8, y + 8, 3);
      break;
    case ST_WORKING: {
      // Spinner: orbit a single dot around the center.
      int frame = (millis() / 150) % 8;
      float ang = frame * PI / 4.0f;
      int dx = (int)(cos(ang) * 4);
      int dy = (int)(sin(ang) * 4);
      display.drawDisc(x + 8 + dx, y + 8 + dy, 1);
      display.drawCircle(x + 8, y + 8, 6);
      break;
    }
    case ST_WAITING: {
      // Blinking exclamation mark.
      bool blink = (millis() / 300) % 2;
      if (blink) {
        display.drawBox(x + 7, y + 3,  3, 7);
        display.drawBox(x + 7, y + 11, 3, 2);
      }
      break;
    }
    case ST_DONE:
      // Bold checkmark.
      display.drawLine(x + 3, y + 8,  x + 7,  y + 12);
      display.drawLine(x + 7, y + 12, x + 13, y + 4);
      display.drawLine(x + 3, y + 9,  x + 7,  y + 13);
      display.drawLine(x + 7, y + 13, x + 13, y + 5);
      break;
    case ST_ERROR:
      // Bold X.
      display.drawLine(x + 3,  y + 3,  x + 13, y + 13);
      display.drawLine(x + 13, y + 3,  x + 3,  y + 13);
      display.drawLine(x + 4,  y + 3,  x + 13, y + 12);
      display.drawLine(x + 12, y + 3,  x + 3,  y + 12);
      break;
  }
}

// ============== Crypto price fetch ==============
// `data-api.binance.vision` is Binance's public read-only data mirror, fronted
// by AWS CloudFront. It's reachable from mainland China without a VPN, unlike
// `api.binance.com` which is blocked by the GFW. Per-symbol ticker keeps the
// response tiny (~50 bytes) so memory and bandwidth pressure are negligible.
void fetchCoinsOnce() {
  if (WiFi.status() != WL_CONNECTED) return;
  coinsLastTryAt = millis();

  bool gotAny = false;
  for (int i = 0; i < N_COINS; i++) {
    String url = String("https://data-api.binance.vision/api/v3/ticker/price?symbol=")
               + coins[i].sym + "USDT";

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    HTTPClient http;
    http.setTimeout(10000);
    http.setReuse(false);
    http.setUserAgent("claude-display/1.0 (ESP32-C3)");
    if (!http.begin(client, url)) continue;
    int code = http.GET();
    if (code != 200) {
      Serial.printf("[coins] %s HTTP %d\n", coins[i].sym, code);
      http.end();
      continue;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) continue;
    double v = doc["price"].as<String>().toDouble();
    if (v > 0) { coins[i].usd = v; gotAny = true; }
  }

  if (gotAny) {
    coinsUpdatedAt = millis();
    coinsLastOk    = true;
    Serial.printf("[coins] OK: BTC %.0f  ETH %.2f  SOL %.2f\n",
                  coins[0].usd, coins[1].usd, coins[2].usd);
  } else {
    coinsLastOk = false;
    Serial.println("[coins] fetch failed");
  }
}

void coinsTask(void* /*arg*/) {
  // Initial short delay so WiFi/NTP can settle before the first request.
  vTaskDelay(pdMS_TO_TICKS(5000));
  for (;;) {
    fetchCoinsOnce();
    vTaskDelay(pdMS_TO_TICKS(BTC_REFRESH_MS));
  }
}

// Full price (no K-shorthand). Decimal precision scales with magnitude so the
// width stays roughly the same.
//   >= 10000    -> "$78105"      (integer; cents are noise when BTC is $78k)
//      1 - 9999 -> "$2177.91"    (cents matter for ETH/SOL range)
//   < 1         -> "$0.4321"     (small alts)
void formatPrice(char* out, size_t n, double v) {
  bool stale = (coinsUpdatedAt == 0) || ((millis() - coinsUpdatedAt) > BTC_STALE_MS);
  if (v <= 0 || stale)        snprintf(out, n, "--");
  else if (v >= 10000.0)      snprintf(out, n, "$%lu",  (unsigned long)(v + 0.5));
  else if (v >= 1.0)          snprintf(out, n, "$%.2f", v);
  else if (v >= 0.01)         snprintf(out, n, "$%.4f", v);
  else                        snprintf(out, n, "$%.6f", v);
}

// ============== Drawing helpers ==============

// UTF-8-safe truncation: if `s` doesn't fit in `maxW` at the current font,
// trim characters off the end and append an ellipsis. Returns the original
// String if it already fits. Char-boundary aware so multi-byte (e.g. Chinese)
// characters are never chopped mid-sequence.
String fitWidthUtf8(const String& s, int maxW) {
  if (display.getUTF8Width(s.c_str()) <= maxW) return s;
  const char* ellipsis = "...";
  int ellW = display.getUTF8Width(ellipsis);
  if (ellW > maxW) return String();

  String trimmed = s;
  while (trimmed.length() > 0) {
    // Step one UTF-8 character back from the end.
    int i = trimmed.length() - 1;
    while (i > 0 && (uint8_t)trimmed[i] >= 0x80 && (uint8_t)trimmed[i] < 0xC0) i--;
    trimmed = trimmed.substring(0, i);
    String test = trimmed + ellipsis;
    if (display.getUTF8Width(test.c_str()) <= maxW) return test;
  }
  return String(ellipsis);
}

// Top strip: clock left, date right. Shared by clock-eyes and crypto pages so
// both idle screens have the same header.
void drawTopTimeDate() {
  display.setDrawColor(COLOR_FG);
  display.setFont(FONT_TINY);

  struct tm tm;
  bool haveTime = getCurrentLocalTime(&tm);
  char tbuf[12];
  if (haveTime) snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  else          snprintf(tbuf, sizeof(tbuf), "--:--");
  drawText(0, 2, tbuf);

  if (haveTime) {
    char dbuf[16];
    snprintf(dbuf, sizeof(dbuf), "%d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    int dw = display.getStrWidth(dbuf);
    drawText(SCREEN_WIDTH - dw, 2, dbuf);
  }
}

// ============== Drawing ==============
// Takes an int index instead of `Session&` because the Arduino IDE's
// auto-prototype generator inserts the prototype above the struct definition,
// which breaks compilation. Using `int` sidesteps the issue.
void drawSession(int sessionIdx, int ordinal, int total) {
  Session& sn = sessions[sessionIdx];

  // WAITING / ERROR: invert the whole screen on alternating frames so it's
  // hard to miss from across the room.
  bool flash = ((sn.state == ST_WAITING) && ((millis() / 400) % 2)) ||
               ((sn.state == ST_ERROR)   && ((millis() / 250) % 2));
  if (flash) {
    display.setDrawColor(COLOR_FG);
    display.drawBox(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    display.setDrawColor(COLOR_BG);
  } else {
    display.setDrawColor(COLOR_FG);
  }

  // Top: icon + state name.
  drawIcon(sn.state, 2, 0);
  display.setFont(FONT_LRG);
  drawText(22, 0, stateName(sn.state));

  // Top-right: elapsed time / counter.
  display.setFont(FONT_TINY);
  char rbuf[16] = {0};
  if (sn.state == ST_WORKING && sn.workingStartedAt > 0) {
    snprintf(rbuf, sizeof(rbuf), "%lus",
             (unsigned long)((millis() - sn.workingStartedAt) / 1000));
  } else if (sn.state == ST_DONE && sn.lastDurationMs > 0) {
    snprintf(rbuf, sizeof(rbuf), "%lus", (unsigned long)(sn.lastDurationMs / 1000));
  } else if (sn.promptCount > 0) {
    snprintf(rbuf, sizeof(rbuf), "#%lu", (unsigned long)sn.promptCount);
  }
  if (rbuf[0]) {
    int w = display.getStrWidth(rbuf);
    drawText(SCREEN_WIDTH - w, 4, rbuf);
  }

  // Divider.
  display.drawHLine(0, 17, SCREEN_WIDTH);

  // Project name (UTF-8 OK), session hash (4 chars), and rotation indicator.
  int rightCursor = SCREEN_WIDTH;
  display.setFont(FONT_TINY);
  if (total > 1) {
    char idx[12];
    snprintf(idx, sizeof(idx), "[%d/%d]", ordinal, total);
    int w = display.getStrWidth(idx);
    drawText(SCREEN_WIDTH - w, 20, idx);
    rightCursor = SCREEN_WIDTH - w - 2;
  }
  if (sn.id.length() > 0) {
    String h = sn.id.substring(0, 4);
    int w = display.getStrWidth(h.c_str());
    drawText(rightCursor - w, 20, h);
    rightCursor -= w + 2;
  }
  if (sn.project.length() > 0) {
    display.setFont(FONT_CN_12);
    // rightCursor is the leftmost x of the top-right cluster; the project
    // gets all the space to the left of it.
    int projMaxW = rightCursor > 0 ? rightCursor : SCREEN_WIDTH;
    drawText(0, 19, fitWidthUtf8(sn.project, projMaxW));
  }

  // Message area: DONE shows large "took Xs"; long messages get truncated with
  // an ellipsis so the read is still legible.
  display.setFont(FONT_CN_12);
  String m = sn.msg;
  if (sn.state == ST_DONE && m.length() == 0 && sn.lastDurationMs > 0) {
    char dbuf[32];
    uint32_t sec = sn.lastDurationMs / 1000;
    if (sec < 60)
      snprintf(dbuf, sizeof(dbuf), "took %lus", (unsigned long)sec);
    else
      snprintf(dbuf, sizeof(dbuf), "took %lum %lus",
               (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    display.setFont(FONT_LRG);
    int w = display.getStrWidth(dbuf);
    drawText((SCREEN_WIDTH - w) / 2, 32, dbuf);
  } else if (m.length() > 0) {
    drawText(0, 34, fitWidthUtf8(m, SCREEN_WIDTH));
  }

  // Footer: clock + today's stats (or IP / "no wifi" while NTP is syncing).
  display.setFont(FONT_TINY);
  struct tm tm;
  if (getCurrentLocalTime(&tm)) {
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d  %lu msgs  %lu min",
             tm.tm_hour, tm.tm_min,
             (unsigned long)stats.prompts,
             (unsigned long)(stats.workingMs / 60000));
    drawText(0, 56, tbuf);
  } else if (WiFi.status() == WL_CONNECTED) {
    drawText(0, 56, WiFi.localIP().toString());
  } else {
    drawText(0, 56, "no wifi");
  }
}

// Clock + eyes — minimalist. Shared header on top, robot eyes fill the rest.
void drawClockEyesPage() {
  drawTopTimeDate();
  // Robot eyes in the y=14..63 band (adapter applies ROBO_YOFF offset).
  roboEyes.update();
  display.setDrawColor(COLOR_FG);
}

// Crypto page — same top header as the eyes page, then 3 coin rows below.
// Symbol left, USD price right-aligned, 14-px bold for readability.
void drawCryptoPage() {
  drawTopTimeDate();

  display.setDrawColor(COLOR_FG);
  display.setFont(FONT_LRG);

  // Rows under the 12 px top strip: 14..28 / 30..44 / 46..60.
  const int rowY[3] = { 15, 31, 47 };
  for (int i = 0; i < N_COINS && i < 3; i++) {
    drawText(0, rowY[i], coins[i].sym);
    char pbuf[16];
    formatPrice(pbuf, sizeof(pbuf), coins[i].usd);
    int pw = display.getStrWidth(pbuf);
    drawText(SCREEN_WIDTH - pw, rowY[i], pbuf);
  }
}

// Cycle through idle pages on a fixed schedule so the user always sees
// everything within ~20 s of glancing at the device.
void drawIdleScreen() {
  const uint32_t cycle = IDLE_EYES_MS + IDLE_CRYPTO_MS;
  uint32_t       phase = millis() % cycle;
  if (phase < IDLE_EYES_MS) drawClockEyesPage();
  else                       drawCryptoPage();
}

void drawScreen() {
  display.clearBuffer();

  if (inApMode) {
    drawApSetupScreen();
  } else {
    evictStaleSessions();
    int total = 0, ordinal = 0;
    int idx = pickDisplaySession(&ordinal, &total);
    if (idx < 0) drawIdleScreen();
    else         drawSession(idx, ordinal, total);
  }
  display.sendBuffer();
}

// ============== HTTP handlers ==============
void handleStatus() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "POST only");
    return;
  }
  String body = server.arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", String("bad json: ") + err.c_str());
    return;
  }

  String stateStr = doc["state"]   | "idle";
  String msg      = doc["msg"]     | "";
  String project  = doc["project"] | "";
  String sid      = doc["session"] | "default";

  State newState = parseState(stateStr);

  int idx = findOrAllocSession(sid);
  Session& sn = sessions[idx];

  // Track WORKING entry / exit transitions so we can record duration and
  // update today's stats once per real prompt.
  if (newState == ST_WORKING && sn.state != ST_WORKING) {
    sn.workingStartedAt = millis();
    sn.promptCount++;
    stats.prompts++;
    statsSaveThrottled();
  } else if (newState != ST_WORKING && sn.state == ST_WORKING) {
    if (sn.workingStartedAt > 0) {
      uint32_t dur = millis() - sn.workingStartedAt;
      sn.lastDurationMs = dur;
      stats.workingMs += dur;
      statsSaveThrottled();
    }
    sn.workingStartedAt = 0;
  }

  sn.state     = newState;
  sn.msg       = msg;
  if (project.length() > 0) sn.project = project;
  sn.updatedAt = millis();

  Serial.printf("[/status] sid=%s state=%s | %s | %s\n",
                sid.c_str(), stateName(newState),
                sn.project.c_str(), msg.c_str());

  server.send(200, "application/json", "{\"ok\":true}");
}

void handleGet() {
  JsonDocument doc;
  doc["count"] = activeSessionCount();
  JsonArray arr = doc["sessions"].to<JsonArray>();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) continue;
    JsonObject o = arr.add<JsonObject>();
    o["id"]       = sessions[i].id;
    o["state"]    = stateName(sessions[i].state);
    o["msg"]      = sessions[i].msg;
    o["project"]  = sessions[i].project;
    o["age_ms"]   = (uint32_t)(millis() - sessions[i].updatedAt);
    o["prompts"]  = sessions[i].promptCount;
  }
  JsonObject st = doc["stats"].to<JsonObject>();
  st["prompts"]   = stats.prompts;
  st["workingMs"] = stats.workingMs;
  st["day"]       = stats.day;
  JsonObject co = doc["coins"].to<JsonObject>();
  for (int i = 0; i < N_COINS; i++) {
    co[coins[i].sym] = coins[i].usd;
  }
  co["updatedAgo"] = coinsUpdatedAt ? (uint32_t)(millis() - coinsUpdatedAt) : 0;
  co["lastOk"]     = coinsLastOk;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /clear — drop all active sessions. Useful for cleaning up orphans
// from testing without restarting the device.
void handleClearSessions() {
  int n = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].active) { sessions[i].active = false; n++; }
  }
  String resp = "{\"ok\":true,\"cleared\":" + String(n) + "}";
  server.send(200, "application/json", resp);
}

void handleRoot() {
  String html = "<h1>Claude Code Display</h1>";
  html += "<p>Active sessions: <b>" + String(activeSessionCount()) + "</b></p>";
  html += "<p>Today: " + String(stats.prompts) + " prompts, "
       +  String(stats.workingMs / 60000) + " min working</p>";
  html += "<ul>";
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) continue;
    html += "<li>" + sessions[i].id + " &mdash; " + stateName(sessions[i].state)
         +  " &mdash; " + sessions[i].project + " &mdash; " + sessions[i].msg + "</li>";
  }
  html += "</ul>";
  html += "<p>POST /status JSON: {state,msg,project,session}</p>";
  html += "<p>POST /clear &mdash; drop all active sessions</p>";
  html += "<p><a href=\"/wifi\">Change WiFi</a> &middot; <a href=\"/tz\">Change timezone</a></p>";
  server.send(200, "text/html", html);
}

// ============== WiFi ==============
// Two credential sources, tried in order:
//   1. NVS-stored (set via the captive-portal web form)
//   2. Build-time defaults from config.h (so power users / first-time flash
//      with creds prefilled still works)
// If both fail we drop into AP mode and serve the captive portal.

String wifiApName() {
  String mac = WiFi.macAddress();   // "AA:BB:CC:DD:EE:FF"
  mac.replace(":", "");
  return "claude-display-" + mac.substring(8);   // last 4 hex chars
}

// ESP32-C3 Super Mini quirks (see project README "Troubleshooting"):
//   - persistent(false): avoid stale credentials in NVS managed by IDF
//   - disconnect(true,true): fully reset the WiFi subsystem
//   - setSleep(false): keeps the HTTP server snappy
//   - setMinSecurity(OPEN): sidesteps WPA3-only negotiation issues
//   - setTxPower(8.5dBm): avoids RF resets when USB power is marginal
void wifiCommonRadioConfig() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.setMinSecurity(WIFI_AUTH_OPEN);
  // Use a healthier TX power than the historical 8.5 dBm cap — TLS handshakes
  // need ~30 packets and the lower setting was causing outbound HTTPS to drop
  // mid-handshake. 17 dBm is comfortably below brownout threshold on USB.
  WiFi.setTxPower(WIFI_POWER_17dBm);
}

bool tryConnectWith(const String& ssid, const String& pwd, uint32_t timeoutMs) {
  if (ssid.length() == 0) return false;

  display.clearBuffer();
  display.setDrawColor(COLOR_FG);
  display.setFont(FONT_TINY);
  drawText(0, 0,  "WiFi connecting...");
  drawText(0, 12, ssid);
  display.sendBuffer();

  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);
  wifiCommonRadioConfig();
  Serial.printf("STA -> %s (MAC %s)\n", ssid.c_str(), WiFi.macAddress().c_str());
  WiFi.begin(ssid.c_str(), pwd.c_str());

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    // Many CN consumer routers (Xiaomi etc.) hijack DNS or return polluted
    // results that point HTTPS endpoints to the wrong IP, killing TLS.
    // Force clean DNS (AliDNS + DNSPod, both fast inside China).
    WiFi.setDNS(IPAddress(223, 5, 5, 5), IPAddress(119, 29, 29, 29));
    Serial.printf("DNS overridden -> %s / %s\n",
                  WiFi.dnsIP(0).toString().c_str(),
                  WiFi.dnsIP(1).toString().c_str());
    return true;
  }
  return false;
}

void startApPortal() {
  inApMode = true;
  String apName = wifiApName();
  Serial.printf("Starting setup AP: %s\n", apName.c_str());

  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_AP_STA);          // AP_STA lets us scan while AP is up
  wifiCommonRadioConfig();
  WiFi.softAP(apName.c_str());     // open AP, no password
  delay(300);
  IPAddress apIp = WiFi.softAPIP();
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIp);  // captive-portal: every name -> our IP
  Serial.printf("AP IP: %s\n", apIp.toString().c_str());
}

void setupWiFi() {
  String nvsSsid = prefs.getString("wifi_ssid", "");
  String nvsPwd  = prefs.getString("wifi_pwd",  "");
  String cfgSsid = WIFI_SSID;
  String cfgPwd  = WIFI_PASSWORD;

  bool ok = false;
  if (nvsSsid.length() > 0) {
    ok = tryConnectWith(nvsSsid, nvsPwd, 20000);
  }
  if (!ok && cfgSsid.length() > 0 && cfgSsid != nvsSsid) {
    ok = tryConnectWith(cfgSsid, cfgPwd, 20000);
  }

  if (ok) {
    Serial.printf("WiFi OK, IP=%s\n", WiFi.localIP().toString().c_str());
    display.clearBuffer();
    display.setDrawColor(COLOR_FG);
    display.setFont(FONT_TINY);
    drawText(0, 0,  "WiFi OK");
    drawText(0, 12, WiFi.localIP().toString());
    display.sendBuffer();
    delay(600);
  } else {
    Serial.println("WiFi FAILED — entering setup portal");
    startApPortal();
  }
}

// HTML escaping for the few places we echo user-supplied strings.
String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '&')  out += "&amp;";
    else if (c == '<')  out += "&lt;";
    else if (c == '>')  out += "&gt;";
    else if (c == '"')  out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else                out += c;
  }
  return out;
}

void handleWifiPage() {
  String currentSsid = prefs.getString("wifi_ssid", "");
  if (currentSsid.length() == 0) currentSsid = WIFI_SSID;
  int currentTzSec = prefs.getInt("tz_offset", TZ_OFFSET_SEC);
  int currentTzH   = currentTzSec / 3600;
  String mode = inApMode ? "Setup mode (AP)" : ("Connected to " + WiFi.SSID());

  // Page kept small: scanning is async via /wifi/scan to keep this fast.
  String html;
  html.reserve(2400);
  html += F("<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Claude Display WiFi</title><style>"
            "body{font-family:system-ui,sans-serif;max-width:420px;margin:24px auto;"
            "padding:0 16px;color:#222}h1{font-size:20px}label{display:block;"
            "margin:14px 0 6px}input,select{width:100%;padding:10px;font-size:16px;"
            "border-radius:8px;border:1px solid #aaa;box-sizing:border-box}"
            "button{width:100%;padding:12px;font-size:16px;background:#2864ff;"
            "color:#fff;border:0;border-radius:8px;margin-top:18px}"
            "button.danger{background:#cc3333;margin-top:8px}"
            ".muted{color:#666;font-size:13px}.row{display:flex;gap:8px}"
            ".row select{flex:1}.row button{width:auto;padding:8px 12px;"
            "margin:0;background:#555}</style></head><body>");
  html += F("<h1>Claude Display - WiFi</h1>");
  html += "<p class=\"muted\">" + htmlEscape(mode) + "</p>";
  html += F("<form method=\"POST\" action=\"/wifi\">"
            "<label>Network<div class=\"row\">"
            "<select name=\"ssid\" id=\"ssid\"><option>(scanning...)</option></select>"
            "<button type=\"button\" onclick=\"rescan()\">Rescan</button></div></label>"
            "<label>Or type manually<input type=\"text\" name=\"ssid_manual\" "
            "placeholder=\"SSID\"></label>"
            "<label>Password<input type=\"password\" name=\"pwd\" "
            "placeholder=\"(leave blank for open)\"></label>"
            "<label>Timezone (hours from UTC)<input type=\"number\" name=\"tz_h\" "
            "step=\"1\" min=\"-12\" max=\"14\" value=\"");
  html += String(currentTzH);
  html += F("\" placeholder=\"e.g. 8 for China, -5 for US East\"></label>"
            "<button type=\"submit\">Save and reboot</button></form>"
            "<form method=\"POST\" action=\"/wifi/forget\" "
            "onsubmit=\"return confirm('Forget saved WiFi?')\">"
            "<button class=\"danger\" type=\"submit\">Forget saved WiFi</button></form>"
            "<p class=\"muted\">Saved: ");
  html += htmlEscape(currentSsid.length() ? currentSsid : String("(none)"));
  html += F("</p><script>"
            "async function rescan(){const s=document.getElementById('ssid');"
            "s.innerHTML='<option>(scanning...)</option>';"
            "try{const r=await fetch('/wifi/scan');const j=await r.json();"
            "s.innerHTML='';if(!j.networks||!j.networks.length){"
            "s.innerHTML='<option>(none found)</option>';return}"
            "j.networks.forEach(n=>{const o=document.createElement('option');"
            "o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm'+"
            "(n.open?'':' lock')+')';s.appendChild(o)});}"
            "catch(e){s.innerHTML='<option>(scan failed)</option>'}}"
            "window.addEventListener('load',rescan);"
            "</script></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleWifiScan() {
  // Blocking scan; ESP32 needs ~1.5–3 s for a 2.4 GHz channel sweep.
  int n = WiFi.scanNetworks(false, true, false, 300);
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleWifiSave() {
  String ssid = server.arg("ssid_manual");
  ssid.trim();
  if (ssid.length() == 0) ssid = server.arg("ssid");
  String pwd  = server.arg("pwd");
  if (ssid.length() == 0) {
    server.send(400, "text/plain", "ssid required");
    return;
  }
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pwd",  pwd);

  String tzStr = server.arg("tz_h");
  if (tzStr.length() > 0) {
    int tzH = tzStr.toInt();
    if (tzH >= -12 && tzH <= 14) {
      prefs.putInt("tz_offset", tzH * 3600);
    }
  }
  String resp = "<!doctype html><meta charset=\"utf-8\"><body style=\"font-family:system-ui;margin:24px\">"
                "<h2>Saved. Rebooting...</h2><p>Joining <b>" + htmlEscape(ssid) +
                "</b>. If it works the display shows the IP. Reconnect your phone to your usual WiFi.</p>";
  server.send(200, "text/html; charset=utf-8", resp);
  delay(800);
  ESP.restart();
}

// GET /tz — small page to change timezone alone, without re-entering WiFi.
// POST /tz with tz_h=<int> persists it to NVS and applies immediately
// (re-runs configTime so the new offset is live without a reboot).
void handleTzPage() {
  int currentTzH = prefs.getInt("tz_offset", TZ_OFFSET_SEC) / 3600;
  String html;
  html.reserve(900);
  html += F("<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Timezone</title><style>"
            "body{font-family:system-ui,sans-serif;max-width:360px;margin:24px auto;"
            "padding:0 16px;color:#222}h1{font-size:20px}label{display:block;"
            "margin:14px 0 6px}input{width:100%;padding:10px;font-size:16px;"
            "border-radius:8px;border:1px solid #aaa;box-sizing:border-box}"
            "button{width:100%;padding:12px;font-size:16px;background:#2864ff;"
            "color:#fff;border:0;border-radius:8px;margin-top:18px}"
            ".muted{color:#666;font-size:13px}</style></head><body>"
            "<h1>Timezone</h1>"
            "<form method=\"POST\" action=\"/tz\">"
            "<label>Hours from UTC<input type=\"number\" name=\"tz_h\" step=\"1\" "
            "min=\"-12\" max=\"14\" value=\"");
  html += String(currentTzH);
  html += F("\"></label><button type=\"submit\">Save</button></form>"
            "<p class=\"muted\">Applied immediately, no reboot.</p>"
            "</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleTzSave() {
  String tzStr = server.arg("tz_h");
  if (tzStr.length() == 0) {
    server.send(400, "text/plain", "tz_h required");
    return;
  }
  int tzH = tzStr.toInt();
  if (tzH < -12 || tzH > 14) {
    server.send(400, "text/plain", "tz_h out of range (-12..14)");
    return;
  }
  prefs.putInt("tz_offset", tzH * 3600);
  // Apply live — NTP will re-anchor to the new offset on the next tick.
  if (WiFi.status() == WL_CONNECTED) {
    configTime(tzH * 3600, 0, NTP_SERVER1, NTP_SERVER2);
  }
  Serial.printf("[tz] saved UTC%+d\n", tzH);
  String resp = "<!doctype html><meta charset=\"utf-8\"><body style=\"font-family:system-ui;margin:24px\">"
                "<h2>Timezone set to UTC" + String(tzH >= 0 ? "+" : "") + String(tzH) +
                "</h2><p><a href=\"/\">Back</a></p></body>";
  server.send(200, "text/html; charset=utf-8", resp);
}

void handleWifiForget() {
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pwd");
  prefs.remove("tz_offset");
  server.send(200, "text/html; charset=utf-8",
              "<!doctype html><meta charset=\"utf-8\"><body style=\"font-family:system-ui;margin:24px\">"
              "<h2>Forgotten. Rebooting.</h2></body>");
  delay(800);
  ESP.restart();
}

// In AP mode, any unknown URL gets redirected to /wifi so iOS / Android
// captive-portal probes pop the setup page automatically.
void handleNotFound() {
  if (inApMode) {
    server.sendHeader("Location", "/wifi", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "not found");
  }
}

// Idle screen replacement when we're stuck in AP setup mode.
void drawApSetupScreen() {
  String apName = wifiApName();
  IPAddress ip  = WiFi.softAPIP();

  display.setDrawColor(COLOR_FG);
  display.setFont(FONT_LRG);
  drawText(0, 0, "WiFi setup");

  display.setFont(FONT_TINY);
  drawText(0, 18, "Phone -> AP:");
  drawText(0, 28, apName);
  drawText(0, 42, "Open in browser:");
  drawText(0, 52, ip.toString());
}

// ============== Setup / Loop ==============
void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(I2C_SDA, I2C_SCL);
  display.setI2CAddress(SCREEN_ADDRESS << 1);  // u8g2 wants 8-bit write address
  display.begin();
  display.enableUTF8Print();
  display.setBusClock(400000);
  display.clearBuffer();
  display.sendBuffer();

  prefs.begin("claude", false);
  statsLoad();

  setupWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS: http://%s.local\n", MDNS_NAME);
    }
    int tzSec = prefs.getInt("tz_offset", TZ_OFFSET_SEC);
    configTime(tzSec, 0, NTP_SERVER1, NTP_SERVER2);
    Serial.printf("TZ offset = %d sec (UTC%+d)\n", tzSec, tzSec / 3600);
  }

  // RoboEyes: idle screen "filler". Passing the eye-band height (not the
  // physical 64 px) keeps the library's idle-gaze randomizer inside our band.
  // Eyes scaled up now that the bottom text strip is gone.
  roboEyes.begin(SCREEN_WIDTH, ROBO_BAND_HEIGHT, 50);  // 50 fps cap (we drive at ~25 fps)
  roboEyes.setWidth(26, 26);
  roboEyes.setHeight(26, 26);
  roboEyes.setSpacebetween(14);
  roboEyes.setBorderradius(8, 8);
  roboEyes.setPosition(DEFAULT);
  roboEyes.setAutoblinker(ON, 3, 2);   // blink every 3..5 s
  roboEyes.setIdleMode(ON, 2, 3);      // re-aim every 2..5 s
  roboEyes.setCuriosity(true);          // outer eye widens when looking sideways

  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/status",      HTTP_POST, handleStatus);
  server.on("/status",      HTTP_GET,  handleGet);
  server.on("/clear",       HTTP_POST, handleClearSessions);
  server.on("/wifi",        HTTP_GET,  handleWifiPage);
  server.on("/wifi",        HTTP_POST, handleWifiSave);
  server.on("/wifi/scan",   HTTP_GET,  handleWifiScan);
  server.on("/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/tz",          HTTP_GET,  handleTzPage);
  server.on("/tz",          HTTP_POST, handleTzSave);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server up on :80");

  // Background task: fetch BTC/ETH/SOL prices every minute. ESP32-C3 is
  // single-core, so pin to core 0; the OS scheduler multiplexes with the main
  // loop. TLS handshake transiently needs a roomy stack — 16 KB is comfortable.
  if (WiFi.status() == WL_CONNECTED) {
    // Bump stack to 24 KB — TLS handshake + HTTPClient string ops can push past
    // 16 KB on chained allocations.
    xTaskCreatePinnedToCore(coinsTask, "coins", 24576, nullptr, 1, nullptr, 0);
  }
}

void loop() {
  server.handleClient();
  if (inApMode) dnsServer.processNextRequest();

  // WORKING with no fresh updates for too long: assume the user interrupted
  // or a Stop hook didn't fire. Auto-revert to IDLE so the screen frees up.
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) continue;
    if (sessions[i].state == ST_WORKING &&
        (millis() - sessions[i].updatedAt) > WORKING_TIMEOUT_MS) {
      if (sessions[i].workingStartedAt > 0) {
        uint32_t dur = millis() - sessions[i].workingStartedAt;
        sessions[i].lastDurationMs = dur;
        stats.workingMs += dur;
        statsSaveThrottled();
      }
      sessions[i].state            = ST_IDLE;
      sessions[i].msg              = "(timeout)";
      sessions[i].workingStartedAt = 0;
      sessions[i].updatedAt        = millis();
    }
  }

  // Once per minute: if the local date rolled over, reset today's stats.
  static uint32_t lastRolloverCheck = 0;
  if (millis() - lastRolloverCheck > 60000) {
    lastRolloverCheck = millis();
    statsCheckRollover();
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > DRAW_INTERVAL_MS) {
    lastDraw = millis();
    drawScreen();
  }
}
