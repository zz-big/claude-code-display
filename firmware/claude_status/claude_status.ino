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

#include "config.h"

#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

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
const uint32_t WORKING_TIMEOUT_MS   = 60000;                  // WORKING auto-revert -> IDLE
const uint32_t DONE_LINGER_MS       = 5000;                   // DONE shows briefly then yields

// ============== State enum ==============
// Must precede every function — the Arduino IDE inserts auto-generated
// prototypes before the first function, so any type used in a signature has
// to be declared before that point.
enum State { ST_IDLE, ST_WORKING, ST_WAITING, ST_DONE, ST_ERROR };

// ============== Globals ==============
// u8g2 full-buffer, hardware I2C using the pins set on Wire above
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
WebServer        server(80);
Preferences      prefs;

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

int findOrAllocSession(const String& id) {
  int idx = findSession(id);
  if (idx >= 0) return idx;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) {
      sessions[i].id               = id;
      sessions[i].project          = "";
      sessions[i].state            = ST_IDLE;
      sessions[i].msg              = "";
      sessions[i].updatedAt        = millis();
      sessions[i].workingStartedAt = 0;
      sessions[i].lastDurationMs   = 0;
      sessions[i].promptCount      = 0;
      sessions[i].active           = true;
      return i;
    }
  }
  // All slots taken: replace the least-recently-updated one.
  int oldest = 0;
  for (int i = 1; i < MAX_SESSIONS; i++) {
    if (sessions[i].updatedAt < sessions[oldest].updatedAt) oldest = i;
  }
  sessions[oldest].id               = id;
  sessions[oldest].project          = "";
  sessions[oldest].state            = ST_IDLE;
  sessions[oldest].msg              = "";
  sessions[oldest].updatedAt        = millis();
  sessions[oldest].workingStartedAt = 0;
  sessions[oldest].lastDurationMs   = 0;
  sessions[oldest].promptCount      = 0;
  sessions[oldest].active           = true;
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
    drawText(0, 19, sn.project);
  }

  // Message area: DONE shows large "took Xs"; long messages scroll horizontally.
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
    int textW = display.getUTF8Width(m.c_str());
    if (textW <= SCREEN_WIDTH) {
      drawText(0, 34, m);
    } else {
      int totalW = textW + SCREEN_WIDTH;
      int offset = (millis() / 50) % totalW;   // ~20 px/s scroll
      drawText(SCREEN_WIDTH - offset, 34, m);
    }
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

void drawIdleScreen() {
  display.setDrawColor(COLOR_FG);

  struct tm tm;
  bool haveTime = getCurrentLocalTime(&tm);

  // Big centered clock.
  if (haveTime) {
    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    display.setFont(FONT_BIG_NUM);
    int w = display.getStrWidth(tbuf);
    drawText((SCREEN_WIDTH - w) / 2, 0, tbuf);

    display.setFont(FONT_TINY);
    char dbuf[16];
    snprintf(dbuf, sizeof(dbuf), "%d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    int dw = display.getStrWidth(dbuf);
    drawText((SCREEN_WIDTH - dw) / 2, 26, dbuf);
  } else {
    display.setFont(FONT_LRG);
    int cw = display.getStrWidth("CLAUDE");
    drawText((SCREEN_WIDTH - cw) / 2, 4, "CLAUDE");
    display.setFont(FONT_TINY);
    drawText(20, 26, "(syncing time)");
  }

  // Today's stats (one line to leave room).
  display.setFont(FONT_CN_12);
  char sbuf[40];
  snprintf(sbuf, sizeof(sbuf), "%lu prompts  %lu min",
           (unsigned long)stats.prompts,
           (unsigned long)(stats.workingMs / 60000));
  drawText(0, 35, sbuf);

  // Footer: IP on the left, uptime on the right.
  display.setFont(FONT_TINY);
  if (WiFi.status() == WL_CONNECTED) {
    drawText(0, 56, WiFi.localIP().toString());
  } else {
    drawText(0, 56, "no wifi");
  }

  uint32_t up = millis() / 1000;
  char ubuf[20];
  if (up < 3600)        snprintf(ubuf, sizeof(ubuf), "up %lum", (unsigned long)(up / 60));
  else if (up < 86400)  snprintf(ubuf, sizeof(ubuf), "up %luh%lum",
                                 (unsigned long)(up / 3600),
                                 (unsigned long)((up % 3600) / 60));
  else                  snprintf(ubuf, sizeof(ubuf), "up %lud", (unsigned long)(up / 86400));
  int uw = display.getStrWidth(ubuf);
  drawText(SCREEN_WIDTH - uw, 56, ubuf);
}

void drawScreen() {
  display.clearBuffer();
  evictStaleSessions();

  int total = 0, ordinal = 0;
  int idx = pickDisplaySession(&ordinal, &total);

  if (idx < 0) {
    drawIdleScreen();
  } else {
    drawSession(idx, ordinal, total);
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
  StaticJsonDocument<512> doc;
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
  StaticJsonDocument<2048> doc;
  doc["count"] = activeSessionCount();
  JsonArray arr = doc.createNestedArray("sessions");
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].active) continue;
    JsonObject o = arr.createNestedObject();
    o["id"]       = sessions[i].id;
    o["state"]    = stateName(sessions[i].state);
    o["msg"]      = sessions[i].msg;
    o["project"]  = sessions[i].project;
    o["age_ms"]   = (uint32_t)(millis() - sessions[i].updatedAt);
    o["prompts"]  = sessions[i].promptCount;
  }
  JsonObject st = doc.createNestedObject("stats");
  st["prompts"]   = stats.prompts;
  st["workingMs"] = stats.workingMs;
  st["day"]       = stats.day;
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
  server.send(200, "text/html", html);
}

// ============== WiFi ==============
void setupWiFi() {
  display.clearBuffer();
  display.setDrawColor(COLOR_FG);
  display.setFont(FONT_TINY);
  drawText(0, 0,  "WiFi connecting...");
  drawText(0, 12, WIFI_SSID);
  display.sendBuffer();

  // ESP32-C3 Super Mini quirks (see project README "Troubleshooting"):
  //   - persistent(false) avoids stale credentials in NVS
  //   - disconnect(true,true) fully resets the WiFi subsystem
  //   - setSleep(false) keeps the HTTP server snappy
  //   - setMinSecurity(OPEN) sidesteps WPA3-only negotiation issues
  //   - setTxPower(8.5dBm) avoids RF resets when USB power is marginal
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.setMinSecurity(WIFI_AUTH_OPEN);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  Serial.printf("STA MAC = %s\n", WiFi.macAddress().c_str());
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  display.clearBuffer();
  display.setDrawColor(COLOR_FG);
  display.setFont(FONT_TINY);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK, IP=%s\n", WiFi.localIP().toString().c_str());
    drawText(0, 0,  "WiFi OK");
    drawText(0, 12, WiFi.localIP().toString());
  } else {
    Serial.println("WiFi FAILED");
    drawText(0, 0, "WiFi FAIL");
  }
  display.sendBuffer();
  delay(800);
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
    configTime(TZ_OFFSET_SEC, 0, NTP_SERVER1, NTP_SERVER2);
  }

  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/status", HTTP_POST, handleStatus);
  server.on("/status", HTTP_GET,  handleGet);
  server.on("/clear",  HTTP_POST, handleClearSessions);
  server.begin();
  Serial.println("HTTP server up on :80");
}

void loop() {
  server.handleClient();

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
  if (millis() - lastDraw > 100) {
    lastDraw = millis();
    drawScreen();
  }
}
