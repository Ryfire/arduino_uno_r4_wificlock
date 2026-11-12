/*
 * UNO R4 WiFi - Smart Digital Clock with 4-Character Display
 *
 * Features:
 * - NTP-synced real time with automatic US daylight-saving changes
 * - Non-blocking WiFi reconnect: retries forever with backoff
 * - NTP retry backoff + server rotation; clock keeps running offline
 * - Display cycle: sequence order, per-page durations and the scene
 *   schedule are configurable via scene_config.json (defaults: time ->
 *   large minutes -> date -> year -> day-name scroll -> wifi signal, 7 s
 *   each, plus a periodic eyeball animation every Nth cycle)
 * - Optional 24-hour format; timezone configured in secrets.h
 * - State machine: INITIALIZING -> TIME_DISPLAY -> ERROR_DISPLAY
 *
 * Platform: Arduino UNO R4 WiFi
 * Framework: Arduino
 */

#include <Arduino.h>
#include <DimmableR4Matrix.h>
#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
// Bare-name includes so the same file builds under PlatformIO (resolves via
// the -I include build flag) and as an Arduino IDE sketch (resolves from the
// header tabs placed next to the .ino).
#include "glyphFonts.h"
// secrets.h is your private credentials file (gitignored). If it doesn't
// exist (fresh clone), fall back to the example template so the project
// compiles out of the box; copy the example to secrets.h for real credentials.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #include "secrets.example.h"
#endif
// Display sequence (order/durations/on-off) and the scene schedule come from
// scene_config.h — GENERATED from scene_config.json by
// tools/generate_scene_config.py, which runs automatically before every
// build (see platformio.ini). Edit the JSON, never this file's twin.
#include "scene_config.h"
#include "boot_logo.h"

// ===== WiFi Configuration =====
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ===== Time Configuration (override in secrets.h) =====
// Standard-time offset from UTC in seconds (EST = -5*3600, PST = -8*3600, ...)
#ifndef TZ_BASE_OFFSET
#define TZ_BASE_OFFSET (-5 * 3600)
#endif
// 1 = automatic US daylight saving (2nd Sun Mar -> 1st Sun Nov), 0 = fixed offset
#ifndef TZ_USE_US_DST
#define TZ_USE_US_DST 1
#endif
// 1 = 24-hour display (HHMM), 0 = 12-hour with AM/PM
#ifndef TIME_FORMAT_24H
#define TIME_FORMAT_24H 0
#endif

// ===== Night dimming (instant schedule switch) =====
// Local hours [DIM_START_HOUR, 24) + [0, DIM_END_HOUR) run the whole matrix
// at DIM_NIGHT_PERCENT brightness; the switch is instant, no ramp.
#ifndef DIM_START_HOUR
#define DIM_START_HOUR 22
#endif
#ifndef DIM_END_HOUR
#define DIM_END_HOUR 6
#endif
#ifndef DIM_NIGHT_PERCENT
#define DIM_NIGHT_PERCENT 15
#endif
#ifndef DIM_DAY_PERCENT
#define DIM_DAY_PERCENT 100
#endif

// ===== OTA debug flashing (local-only; NOT part of the GitHub build) =====
// Enabled only by the platformio_ota.ini environment (build flag -DOTA_ENABLED).
// The board listens on port 65280 and PlatformIO POSTs a new firmware over WiFi
// (like wireless ADB). First flash must be USB; after that, every flash is
// wireless. The whole block is inert without the flag, so the GitHub build is
// byte-identical to the normal build.
#ifdef OTA_ENABLED
#include <ArduinoOTA.h>

#ifndef OTA_NAME
#define OTA_NAME "arduino"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "123"
#endif
// Static IP so the upload URL never needs editing. Subnet matches this
// machine's LAN (192.168.12.x); .250 is safely outside the typical DHCP pool.
#ifndef OTA_STATIC_IP
#define OTA_STATIC_IP 192,168,12,250
#endif
#endif

const unsigned long NTP_OK_INTERVAL = 60000;    // regular sync cadence (ms)
const unsigned long STALE_SYNC_MS = 3600000UL;  // no sync for 1h => indicator
// Per-phase durations come from SCENE_SEQUENCE[] in scene_config.h
// (generated from scene_config.json — edit there, not here).
// Per-phase durations and page-INTERNAL animation timing (flip rate, scroll
// speed, wifi pacing, ...) come from SCENE_SEQUENCE[] in scene_config.h
// (generated from scene_config.json — edit there, not here).
const unsigned long DATE_DROP_MS = 70;          // date entrance: ms per fall row
const int DATE_FALL_TOP = 5;                    // date entrance: fall starts this many rows above the matrix
const int COMPACT_ROW_OFFSET = 2;               // compact glyphs rest at frame rows 2-6
const unsigned long SWEEP_MS = 40;              // phase transition: ms per column of the wipe
const unsigned long WARN_X_ON_MS = 450;         // banner: X visible per flash
const unsigned long WARN_X_OFF_MS = 250;        // banner: dark gap between flashes
const int WARN_X_FLASHES = 3;                   // banner: flashes before the marquee
const unsigned long WARN_SCROLL_MS = 90;        // banner: ms per marquee column
const unsigned long WIFI_CONNECT_TIMEOUT = 15000; // per-attempt timeout (ms)

// ===== Time-of-day scene intro (TIME page) =====
// A short vignette plays at the start of every TIME visit before the clock
// is revealed. SCENE_STYLE picks the flavor (override in secrets.h):
//   2 = animated scenes | 1 = static image art | 0 = no intro
// SCENE_FORCE is the bench-preview hook: -1 follows the clock's hour,
// 0-5 forces one scene, 6 cycles all six per visit.
// SCENE_INTRO_MS, the style default, and the hour->scene schedule all live
// in scene_config.h (generated from scene_config.json).
#ifndef SCENE_STYLE
#define SCENE_STYLE CONFIG_SCENE_STYLE_DEFAULT
#endif
#ifndef SCENE_FORCE
#define SCENE_FORCE -1
#endif

// Day names for the scroll page, indexed by day-of-week (0 = Sunday).
// Epoch days map directly: (days + 4) % 7 -> 1970-01-01 was a Thursday.
const char* dayNames[7] = {
  "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY",
  "THURSDAY", "FRIDAY", "SATURDAY"
};

// NTP servers; rotated round-robin on sync failure
const char* ntpServers[] = {"pool.ntp.org", "time.nist.gov", "time.google.com"};
const int ntpServerCount = sizeof(ntpServers) / sizeof(ntpServers[0]);

// ===== WiFi fan icon (signal page) =====
// One cumulative bitmap per animation step: the dot first, then the three
// arcs build up bottom-to-top. 8 rows x 12 cols, fan centered on the
// matrix's center line (cols 5-6). The number of arcs shown maps to
// signal quality (see wifiRssiSteps()).
const uint8_t WIFI_FAN[4][8][12] = {
  // step 0: dot only
  {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0}
  },
  // step 1: dot + innermost arc
  {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,1,0,0,1,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0}
  },
  // step 2: dot + two inner arcs
  {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,1,0,0,0,0,1,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,1,0,0,1,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0}
  },
  // step 3: full fan (all three arcs + dot)
  {
    {0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,1,1,0,0,1,1,0,0,0},
    {0,0,1,0,0,0,0,0,0,1,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,1,0,0,0,0,1,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,1,0,0,1,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0}
  }
};

// Full-matrix X / checkmark, shared by the boot status screen and the
// NO WIFI banner.
const uint8_t CHECKMARK[8][12] = {
  {0,0,0,0,0,0,0,0,0,0,1,0},
  {0,0,0,0,0,0,0,0,0,1,1,0},
  {0,0,0,0,0,0,0,0,1,1,0,0},
  {1,0,0,0,0,0,0,1,1,0,0,0},
  {1,1,0,0,0,0,1,1,0,0,0,0},
  {0,1,1,0,0,1,1,0,0,0,0,0},
  {0,0,1,1,1,1,0,0,0,0,0,0},
  {0,0,0,1,1,0,0,0,0,0,0,0}
};
const uint8_t XMARK[8][12] = {
  {1,0,0,0,0,0,0,0,0,0,0,1},
  {0,1,0,0,0,0,0,0,0,0,1,0},
  {0,0,1,0,0,0,0,0,0,1,0,0},
  {0,0,0,1,0,0,0,0,1,0,0,0},
  {0,0,0,0,1,0,0,1,0,0,0,0},
  {0,0,0,0,0,1,1,0,0,0,0,0},
  {0,0,0,0,1,0,0,1,0,0,0,0},
  {0,0,0,1,0,0,0,0,1,0,0,0}
};

// Static image art for the time-of-day intro (SCENE_STYLE=1), indexed by
// SceneKind: night, sunrise, morning, day, sunset, dusk. Purpose-drawn
// stills — every star lit, every ray on — sharing the animated scenes'
// ground line on row 7. Keep in sync with STATIC_ART in
// tools/render_readme_art.py (the README frames are generated from it).
const uint8_t SCENE_ART[6][8][12] = {
  {   // night: crescent moon + four stars
    {0,0,0,0,1,0,0,0,0,1,0,0},
    {0,0,1,0,0,0,0,0,1,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
    {0,0,0,0,1,0,0,1,1,0,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
    {0,1,0,0,0,0,0,0,1,1,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1}
  },
  {   // sunrise: sun half out of the ground, early stars still up
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,1,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1}
  },
  {   // morning: small sun, all rays on, cloud parked mid-sky
    {0,0,1,0,0,0,0,0,0,0,0,0},
    {0,1,1,1,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0},
    {0,1,1,1,0,0,0,0,0,0,0,0},
    {0,0,1,0,0,0,0,1,1,0,0,0},
    {0,0,0,0,0,0,1,1,1,1,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1}
  },
  {   // day: full sunburst, every ray lit
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,0,0},
    {0,0,0,1,1,1,1,1,1,1,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1}
  },
  {   // sunset: sun sinking at the horizon, two cloud streaks
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,0,0,0,0,1,1,1,1},
    {0,0,0,0,0,0,0,0,1,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1}
  },
  {   // dusk: four stars over the afterglow dome left on the horizon
    {0,0,0,0,1,0,0,0,0,1,0,0},
    {0,0,1,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,1,0,0,0,0,0,0,0,1,0,0},
    {0,0,0,0,1,1,1,1,0,0,0,0},
    {0,0,0,1,1,1,1,1,1,0,0,0},
    {0,0,1,1,1,1,1,1,1,1,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1}
  }
};

// ===== Objects =====
DimmableR4Matrix matrix;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServers[0], TZ_BASE_OFFSET, NTP_OK_INTERVAL);

// ===== Display Frame =====
uint8_t frame[8][12] = {0};
bool colonVisible = true;
unsigned long lastBlinkTime = 0;

// ===== Status =====
bool timeValid = false;       // true once NTP has synced at least once
unsigned long lastNtpSync = 0;
long appliedOffset = TZ_BASE_OFFSET;

// ===== State Machine =====
enum ClockState {
  STATE_INITIALIZING,
  STATE_TIME_DISPLAY,
  STATE_ERROR_DISPLAY
};
ClockState currentState = STATE_INITIALIZING;
unsigned long errorDisplayStart = 0;

// ===== WiFi reconnect FSM =====
enum WifiState {
  WIFI_DISCONNECTED,
  WIFI_CONNECTING,
  WIFI_CONNECTED
};
WifiState wifiState = WIFI_DISCONNECTED;
unsigned long wifiBackoffUntil = 0;   // millis() when next attempt may start
unsigned long wifiConnectStart = 0;
int wifiFailCount = 0;
bool everWifiConnected = false;

// ===== NTP scheduling =====
unsigned long lastNtpAttempt = 0;
int ntpFailCount = 0;
int ntpServerIndex = 0;

// ===== Display phases =====
enum DisplayPhase {
  PHASE_TIME,
  PHASE_MINUTES,
  PHASE_DATE,
  PHASE_YEAR,
  PHASE_DAY,      // day-name scroll
  PHASE_WIFI,     // wifi signal strength (fan animation + RSSI)
  PHASE_EYE       // intermittent eyeball animation
};
DisplayPhase displayPhase = PHASE_TIME;
unsigned long lastPhaseToggle = 0;
int cycleCount = 0;               // full passes through the phase list
unsigned long wifiPageStart = 0;  // wifi page: animation clock (set on entry)
long wifiPageRssi = 0;            // wifi page: RSSI snapshot taken on entry

// ===== Time-of-day scene intro state (TIME page) =====
enum SceneKind { SCENE_NIGHT, SCENE_SUNRISE, SCENE_MORNING, SCENE_DAY, SCENE_SUNSET, SCENE_DUSK };
SceneKind sceneKind = SCENE_NIGHT;  // resolved when the TIME phase is entered
unsigned long sceneStart = 0;       // scene animation clock (set on entry)
bool sceneRevealed = false;         // scene -> clock wipe fired once per visit
int sceneVisit = 0;                 // visit counter (SCENE_FORCE=6 cycling)

// ===== Day-name scroll state =====
uint8_t scrollBuf[96];            // packed columns: bit r = row r (fits WEDNESDAY: 85 cols)
int scrollLen = 0;                // total columns in buffer
int scrollPos = 0;                // current window offset
unsigned long lastScrollTick = 0;

// ===== Eye animation state =====
enum EyePose { EYE_CENTER, EYE_LEFT, EYE_RIGHT, EYE_BLINK };
// Two eyes: center rest, look left/right, then blink once. The show
// loops for the whole phase; the page plays every Nth cycle (eye step's
// every_n_cycles in scene_config.json).
const uint8_t EYE_SEQ[] = {
  EYE_CENTER, EYE_LEFT, EYE_RIGHT, EYE_CENTER,
  EYE_BLINK, EYE_CENTER
};
const int EYE_SEQ_LEN = sizeof(EYE_SEQ) / sizeof(EYE_SEQ[0]);
int eyeStep = 0;                  // index into EYE_SEQ
unsigned long lastEyeStep = 0;

// ===== Sweep transition state =====
uint8_t prevFrame[8][12];       // outgoing frame snapshot
bool sweepActive = false;
int sweepCol = 0;               // boundary: cols < sweepCol show prevFrame
unsigned long sweepStart = 0;

// ===== Date page entrance =====
unsigned long dateEntranceStart = 0;  // fall -> hold -> flip clock, set on entry

// ===== NO WIFI banner state (overrides the display while WiFi is down) =====
uint8_t warnBuf[96];              // packed marquee columns (same packing as the day page)
int warnLen = 0;
int warnPos = 0;
bool warnActive = false;
enum WarnStage { WARN_FLASH, WARN_SCROLL };
WarnStage warnStage = WARN_FLASH;
unsigned long warnStageStart = 0;
unsigned long warnLastTick = 0;

// ===== Function Prototypes =====
// Display
void clearFrame();
void renderCharsIntoFrame(const char* text);
void display4Chars(const char* text);
void displayTime(int hours, int minutes);
void displayScene(unsigned long elapsed);
void displaySceneArt();
SceneKind pickScene();
void displayMinutesLarge(int minutes);
void displayDate();
void displayYear();
void displayDayScroll();
void displayEye();
void displayWifiRssi();
int wifiRssiSteps(int rssi);
void startWarnBanner();
void displayNoWifiWarning();
int packMarquee(const char* text, uint8_t* buf, int bufSize);
void renderFrame();
void startSweep();
// The enabled step showing a phase from the compiled table (NULL if none —
// shouldn't happen with a validated config). Display functions read their
// page-INTERNAL animation timing (flip rate, scroll speed, wifi pacing, ...)
// from this.
const PhaseStep* stepFor(uint8_t phase) {
  for (uint8_t i = 0; i < SCENE_SEQUENCE_LEN; i++) {
    if (SCENE_SEQUENCE[i].phase == phase && SCENE_SEQUENCE[i].enabled)
      return &SCENE_SEQUENCE[i];
  }
  return nullptr;
}
void stampCompactGlyph(char c, int startCol, int rowOffset);
void drawDatePair(int value, int rowOffset);
void drawSlash();
int getDayOfWeek();
void buildDayScroll(const char* name);
void clearDisplay();
void drawColon(bool visible, int col);
void drawIndicators();
void renderCompactChar(char c, int startCol);
void showWiFiStatus(bool connected);
void displayLoadingAnimation();
#if CONFIG_BOOT_LOGO_ENABLED
void displayBootLogo();
#endif

// Time / sync
long getUtcOffset(int64_t utc);
void serviceDst();
bool attemptNtpOnce();
void serviceNtp();

// WiFi
void connectToWiFi();
void serviceWifi();
void startWifiConnect();
void scheduleReconnect();
void onWifiConnected();
#ifdef OTA_ENABLED
void applyStaticIp();
void startOtaIfReady();
#endif

// State handlers
void handleTimeDisplay();
void handleErrorDisplay();

// ===== Calendar helpers (Howard Hinnant algorithms) =====
void epochToDate(int64_t days, int& y, int& m, int& d) {
  days += 719468;
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  int64_t doe = days - era * 146097;                                  // [0, 146096]
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
  int64_t y2 = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              // [0, 365]
  int64_t mp = (5 * doy + 2) / 153;                                   // [0, 11]
  d = (int)(doy - (153 * mp + 2) / 5 + 1);                            // [1, 31]
  m = (int)(mp + (mp < 10 ? 3 : -9));                                 // [1, 12]
  y = (int)(y2 + (m <= 2 ? 1 : 0));
}

int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;                                        // [0, 399]
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;       // [0, 365]
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Weekday for a day number (0 = Sunday); epoch day 0 was a Thursday
int dayOfWeek(int64_t days) {
  return (int)(((days + 4) % 7 + 7) % 7);
}

// ===== US daylight-saving offset for a UTC instant =====
long getUtcOffset(int64_t utc) {
#if TZ_USE_US_DST
  int y, m, d;
  epochToDate(utc / 86400, y, m, d);

  // Second Sunday of March, 02:00 standard time (converted to UTC)
  int mar1 = dayOfWeek(daysFromCivil(y, 3, 1));
  int secondSunMar = 1 + ((7 - mar1) % 7) + 7;
  int64_t dstStart = daysFromCivil(y, 3, secondSunMar) * 86400LL
                   + (int64_t)(2 - TZ_BASE_OFFSET / 3600) * 3600LL;

  // First Sunday of November, 02:00 daylight time (converted to UTC)
  int nov1 = dayOfWeek(daysFromCivil(y, 11, 1));
  int firstSunNov = 1 + ((7 - nov1) % 7);
  int64_t dstEnd = daysFromCivil(y, 11, firstSunNov) * 86400LL
                 + (int64_t)(2 - (TZ_BASE_OFFSET + 3600) / 3600) * 3600LL;

  bool dst = (utc >= dstStart && utc < dstEnd);
  return TZ_BASE_OFFSET + (dst ? 3600 : 0);
#else
  (void)utc;
  return TZ_BASE_OFFSET;
#endif
}

// Re-evaluate the UTC offset; called every loop while time is valid
void serviceDst() {
  if (!timeValid) return;
  int64_t utc = (int64_t)timeClient.getEpochTime() - appliedOffset;
  long want = getUtcOffset(utc);
  if (want != appliedOffset) {
    appliedOffset = want;
    timeClient.setTimeOffset((int)want);
    Serial.print("[Time] UTC offset changed to ");
    Serial.println(want);
  }
}

// ===== Night dimming schedule =====
bool dimActive = false;   // true while inside the night window

// Instant brightness switch at the night-window boundaries. Uses local hours
// from timeClient (timezone/DST already applied). No-op until time is valid.
// force=true re-applies the current level (used after the manual 'd' test).
void serviceDimSchedule(bool force = false) {
  if (!timeValid) return;
  int h = timeClient.getHours();
  bool night = (h >= DIM_START_HOUR) || (h < DIM_END_HOUR);
  if (force || night != dimActive) {
    dimActive = night;
    matrix.setBrightness(night ? DIM_NIGHT_PERCENT : DIM_DAY_PERCENT);
    Serial.print("[Dim] ");
    Serial.print(night ? "night dim " : "full brightness ");
    Serial.print(night ? DIM_NIGHT_PERCENT : DIM_DAY_PERCENT);
    Serial.print("% at hour ");
    Serial.println(h);
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);

  matrix.begin();
  clearFrame();
  renderFrame();

  Serial.println("UNO R4 WiFi Smart Clock");
#if CONFIG_BOOT_LOGO_ENABLED
  // Boot logo splash: plays through the radio connect wait, unconditionally
  // BEFORE connectToWiFi() so it shows even if WiFi associates instantly
  // (cached AP) — its own exit conditions handle that: IP + minimum hold,
  // or the connect timeout.
  Serial.println("Boot logo: playing while the radio connects...");
  displayBootLogo();
#else
  displayLoadingAnimation();
#endif

  // Blocking (bounded) connect at boot; everything after this is non-blocking
  connectToWiFi();

  if (wifiState == WIFI_CONNECTED) {
    Serial.println("NTP Client started");
    display4Chars("SYNC");

    int retries = 0;
    while (!timeValid && retries < 4) {
      lastNtpAttempt = millis();
      if (attemptNtpOnce()) break;
      retries++;
      delay(300);
    }

    if (timeValid) {
      showWiFiStatus(true);
      delay(1000);
    } else {
      Serial.println("Initial time sync failed, will keep trying...");
      display4Chars("WAIT");
      delay(1000);
    }
  } else {
    showWiFiStatus(false);
    delay(2000);
    Serial.println("Running the NO WIFI banner until WiFi connects...");
    display4Chars("FAIL");
    delay(1500);
  }

  lastNtpAttempt = millis();
  lastPhaseToggle = millis();
  lastBlinkTime = millis();
  sceneStart = millis();           // boot lands on the TIME page: play a scene
  sceneKind = pickScene();
  Serial.println("Setup complete. Entering state machine...");
}

// ===== Main Loop - State Machine =====
void loop() {
#ifdef OTA_ENABLED
  ArduinoOTA.poll();   // OTA upload server (debug-only; see platformio_ota.ini)
#endif
  serviceDimSchedule();   // night/day brightness switch (no-op until time is valid)

  // [DimTest hook] Serial 'd' re-runs the brightness sweep on demand, so the
  // patched driver can be verified any time without waiting for the night
  // schedule or catching the boot window. Remove with the boot sweep.
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'd' || c == 'D') {
      Serial.println("[DimTest] Manual brightness sweep (100 50 25 10 5 100)...");
      const uint8_t levels[] = {100, 50, 25, 10, 5, 100};
      for (uint8_t level : levels) {
        matrix.setBrightness(level);
        Serial.print("[DimTest] Brightness: ");
        Serial.println(level);
        delay(1000);
      }
      Serial.println("[DimTest] Sweep done, restoring schedule brightness.");
      serviceDimSchedule(true);   // re-apply night/day level after the manual test
    }
  }

  switch (currentState) {
    case STATE_INITIALIZING:
      // setup() already did the waiting; run the clock and keep syncing
      currentState = STATE_TIME_DISPLAY;
      Serial.println("State: TIME_DISPLAY");
      break;

    case STATE_TIME_DISPLAY:
      serviceWifi();
      handleTimeDisplay();
      break;

    case STATE_ERROR_DISPLAY:
      handleErrorDisplay();
      break;
  }

  delay(100);  // Main loop timing
}

// ===== WiFi reconnect FSM (fully non-blocking) =====
void serviceWifi() {
  if (wifiState == WIFI_CONNECTED) {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.println("[WiFi] Connection lost, reconnecting...");
    wifiState = WIFI_DISCONNECTED;
    wifiBackoffUntil = millis();  // retry on next pass
    return;
  }

  if (wifiState == WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      onWifiConnected();
      return;
    }
    if (millis() - wifiConnectStart >= WIFI_CONNECT_TIMEOUT) {
      Serial.println("[WiFi] Attempt timed out");
      wifiState = WIFI_DISCONNECTED;
      scheduleReconnect();
    }
    return;
  }

  // WIFI_DISCONNECTED: wait out the backoff before retrying
  if ((long)(millis() - wifiBackoffUntil) < 0) return;
  startWifiConnect();
}

void startWifiConnect() {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(ssid);
  WiFi.disconnect();
#ifdef OTA_ENABLED
  applyStaticIp();
#endif
  WiFi.begin(ssid, password);
  wifiConnectStart = millis();
  wifiState = WIFI_CONNECTING;
}

void scheduleReconnect() {
  wifiFailCount++;
  unsigned long wait = 5000;
  if (wifiFailCount == 2) wait = 10000;
  else if (wifiFailCount == 3) wait = 30000;
  else if (wifiFailCount > 3) wait = 60000;
  wifiBackoffUntil = millis() + wait;
  Serial.print("[WiFi] Next attempt in ");
  Serial.print(wait / 1000);
  Serial.println("s");
}

#ifdef OTA_ENABLED
// Apply the static IP (must be called before WiFi.begin()).
void applyStaticIp() {
  IPAddress ip(OTA_STATIC_IP);
  WiFi.config(ip);
  Serial.print("[WiFi] Static IP: ");
  Serial.println(ip);
}

// Start the OTA upload server. Re-begin on every (re)connection so the server
// socket survives a WiFi drop + reconnect.
void startOtaIfReady() {
  if (wifiState != WIFI_CONNECTED) return;
  ArduinoOTA.end();
  ArduinoOTA.begin(WiFi.localIP(), OTA_NAME, OTA_PASSWORD, InternalStorage);
  Serial.print("[OTA] Ready: http://");
  Serial.print(OTA_NAME);
  Serial.print(":");
  Serial.print(OTA_PASSWORD);
  Serial.print("@");
  Serial.print(WiFi.localIP());
  Serial.println(":65280/sketch");
}
#endif

void onWifiConnected() {
  bool reconnected = everWifiConnected;
  wifiState = WIFI_CONNECTED;
  wifiFailCount = 0;
  everWifiConnected = true;

  // The NTP UDP socket dies with the connection: close and rebind it
  if (reconnected) {
    ntpUDP.stop();
  }
  timeClient.begin();

  if (reconnected) {
    // Fresh start against the primary server, sync on the next pass
    ntpFailCount = 0;
    ntpServerIndex = 0;
    timeClient.setPoolServerName(ntpServers[0]);
    lastNtpAttempt = millis() - 900000UL;
  }

  // Restart the page-cycle timer so a long outage doesn't flutter through
  // phases catching up on missed toggles; the pages resume from "now".
  lastPhaseToggle = millis();

  Serial.println("[WiFi] Connected");
  Serial.print("[WiFi] IP Address: ");
  Serial.println(WiFi.localIP());
#ifdef OTA_ENABLED
  startOtaIfReady();
#endif
}

void connectToWiFi() {
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("WiFi module not found!");
    wifiState = WIFI_DISCONNECTED;
    wifiBackoffUntil = millis() + 60000;
    return;
  }

  Serial.print("Connecting to: ");
  Serial.println(ssid);

#ifdef OTA_ENABLED
  applyStaticIp();
#endif
  WiFi.begin(ssid, password);
  wifiConnectStart = millis();
  wifiState = WIFI_CONNECTING;

  while (WiFi.status() != WL_CONNECTED && millis() - wifiConnectStart < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    onWifiConnected();
  } else {
    Serial.println("\nWiFi connection failed at boot, will keep retrying...");
    wifiState = WIFI_DISCONNECTED;
    scheduleReconnect();
  }
}

// ===== NTP sync with backoff and server rotation =====
bool attemptNtpOnce() {
  if (timeClient.update()) {
    if (timeClient.getEpochTime() > 1000000000UL) {
      timeValid = true;
      lastNtpSync = millis();
      ntpFailCount = 0;
      ntpServerIndex = 0;
      timeClient.setPoolServerName(ntpServers[0]);
      serviceDst();
      Serial.print("Time synchronized: ");
      Serial.println(timeClient.getFormattedTime());
      return true;
    }
  }

  // Failure: rotate to the next server
  ntpFailCount++;
  ntpServerIndex = (ntpServerIndex + 1) % ntpServerCount;
  timeClient.setPoolServerName(ntpServers[ntpServerIndex]);
  Serial.print("[NTP] Sync failed (streak ");
  Serial.print(ntpFailCount);
  Serial.print(", next server ");
  Serial.print(ntpServers[ntpServerIndex]);
  Serial.println(")");
  return false;
}

void serviceNtp() {
  if (wifiState != WIFI_CONNECTED) return;

  unsigned long interval;
  if (!timeValid) {
    interval = (ntpFailCount < 3) ? 5000 : 30000;  // first sync: patient, then slower
  } else if (ntpFailCount <= 1) {
    interval = NTP_OK_INTERVAL;
  } else if (ntpFailCount == 2) {
    interval = 300000;   // 5 min
  } else {
    interval = 900000;   // 15 min
  }

  if ((long)(millis() - lastNtpAttempt) < (long)interval) return;
  lastNtpAttempt = millis();

  if (!attemptNtpOnce() && ntpFailCount % 3 == 0) {
    // Blink ERR! every 3rd consecutive failure; the clock keeps running
    Serial.println("[NTP] Entering error display");
    currentState = STATE_ERROR_DISPLAY;
    errorDisplayStart = millis();
  }
}

// ===== Sweep transition =====
// On phase change, snapshot the outgoing frame; renderFrame() then composites
// cols < sweepCol from the snapshot and cols >= sweepCol from the incoming
// page's live frame. The boundary advances left at SWEEP_MS per column.
// (The date page falls in on its own instead — see displayDate.)
void startSweep() {
  memcpy(prevFrame, frame, sizeof(prevFrame));
  sweepActive = true;
  sweepStart = millis();
}

// Renders the current frame with the sweep composited over it (no-op when
// sweepActive is false). Replaces direct matrix.renderBitmap() calls.
void renderFrame() {
  if (sweepActive) {
    unsigned long elapsed = millis() - sweepStart;
    sweepCol = 12 - (int)(elapsed / SWEEP_MS);
    if (sweepCol <= 0) {
      sweepCol = 0;
      sweepActive = false;
    }
    for (int r = 0; r < 8; r++) {
      for (int c = 0; c < sweepCol; c++) {
        frame[r][c] = prevFrame[r][c];
      }
    }
  }
  matrix.renderBitmap(frame, 8, 12);
}

// ===== STATE HANDLER: Time Display =====
// The rotation walks SCENE_SEQUENCE[] (scene_config.h): next enabled step,
// each held for its own duration. With only one step enabled the display
// simply pins to it.
uint8_t sequenceIndex = 0;   // index into SCENE_SEQUENCE[]

uint8_t stepIndexFor(DisplayPhase phase) {
  for (uint8_t i = 0; i < SCENE_SEQUENCE_LEN; i++) {
    if (SCENE_SEQUENCE[i].phase == (uint8_t)phase && SCENE_SEQUENCE[i].enabled) return i;
  }
  return 0;   // config validation guarantees at least one enabled step
}

void advancePhase() {
  if (millis() - lastPhaseToggle < SCENE_SEQUENCE[sequenceIndex].durationMs) return;
  lastPhaseToggle = millis();

  // Next enabled step in schedule order (wrap-around; stays put if the
  // current step is the only enabled one).
  uint8_t nextIdx = sequenceIndex;
  for (uint8_t i = 1; i <= SCENE_SEQUENCE_LEN; i++) {
    uint8_t idx = (sequenceIndex + i) % SCENE_SEQUENCE_LEN;
    if (SCENE_SEQUENCE[idx].enabled) {
      nextIdx = idx;
      break;
    }
  }
  if (nextIdx == sequenceIndex) return;

  DisplayPhase next = (DisplayPhase)SCENE_SEQUENCE[nextIdx].phase;

  // The eye is a periodic page: only run it every Nth cycle. The count
  // still increments on the skip, so "every 5th cycle" holds. A skipped
  // eye falls back to the TIME page, whose duration then applies.
  if (next == PHASE_EYE) {
    cycleCount++;
    uint8_t everyN = SCENE_SEQUENCE[nextIdx].everyNCycles;
    if (everyN == 0 || cycleCount % everyN != 0) {
      next = PHASE_TIME;
      nextIdx = stepIndexFor(PHASE_TIME);
    }
  }

  if (next != displayPhase) {
    // Day-scroll marquees itself in; the date page falls in on its own
    // (see displayDate), so neither of those uses the column wipe.
    if (next != PHASE_DAY && next != PHASE_DATE) {
      startSweep();
    }
    sequenceIndex = nextIdx;
    displayPhase = next;
    if (next == PHASE_DATE) {
      dateEntranceStart = millis();   // restart the fall -> hold -> flip clock
    }
    if (next == PHASE_DAY) {
      scrollPos = 0;
      lastScrollTick = millis();
      buildDayScroll(dayNames[getDayOfWeek()]);   // fresh day name on entry
    }
    if (next == PHASE_EYE) {
      eyeStep = 0;
      lastEyeStep = millis();
    }
    if (next == PHASE_WIFI) {
      wifiPageStart = millis();
      wifiPageRssi = WiFi.RSSI();   // one snapshot per visit; the number stays steady
    }
    if (next == PHASE_TIME) {
      sceneStart = millis();        // scene intro plays before the clock
      sceneRevealed = false;
      sceneKind = pickScene();      // bucket by hour (or SCENE_FORCE)
    }
  }
}

void renderDisplayPhase() {
  advancePhase();

  int hours = timeClient.getHours();    // timezone/DST offset applied
  int minutes = timeClient.getMinutes();

  switch (displayPhase) {
    case PHASE_TIME: {
      // Scene intro first: the vignette runs for SCENE_INTRO_MS, then one
      // sweep wipes the scene frame away to reveal the clock underneath.
      // SCENE_STYLE picks the flavor (see the constants block):
      //   0 = no intro (the page-entry sweep already revealed the clock)
      //   1 = static image art, held still      2 = animated scenes
#if SCENE_STYLE != 0
      unsigned long sceneEl = millis() - sceneStart;
      if (sceneEl < SCENE_INTRO_MS) {
#if SCENE_STYLE == 1
        displaySceneArt();
#else
        displayScene(sceneEl);
#endif
        break;
      }
      if (!sceneRevealed) {
        sceneRevealed = true;
        startSweep();   // prevFrame = last scene render -> clock wipes in
      }
#endif
      const PhaseStep* tcfg = stepFor(GEN_PHASE_TIME);
      if (millis() - lastBlinkTime >= (tcfg ? tcfg->blinkMs : 1000)) {
        colonVisible = !colonVisible;
        lastBlinkTime = millis();
      }
      displayTime(hours, minutes);
      break;
    }
    case PHASE_MINUTES:
      displayMinutesLarge(minutes);
      break;
    case PHASE_DATE:
      displayDate();
      break;
    case PHASE_YEAR:
      displayYear();
      break;
    case PHASE_DAY:
      displayDayScroll();
      break;
    case PHASE_WIFI:
      displayWifiRssi();
      break;
    case PHASE_EYE:
      displayEye();
      break;
  }
}

void handleTimeDisplay() {
  // WiFi down (boot failure or dropout): the warning banner takes over the
  // whole display until the connection is back — serviceWifi() keeps
  // running underneath it (called before this handler in loop()).
  if (wifiState != WIFI_CONNECTED) {
    if (!warnActive) startWarnBanner();
    displayNoWifiWarning();
    return;
  }
  warnActive = false;

  if (timeValid) {
    serviceNtp();
    serviceDst();
    renderDisplayPhase();
  } else {
    // WiFi up but no time yet: keep trying, blink SYNC while waiting
    serviceNtp();
    static unsigned long lastWaitBlink = 0;
    static bool waitVisible = true;
    if (millis() - lastWaitBlink >= 500) {
      waitVisible = !waitVisible;
      lastWaitBlink = millis();
      if (waitVisible) {
        display4Chars("SYNC");
      } else {
        clearDisplay();
      }
    }
  }
}

// ===== STATE HANDLER: Error Display =====
void handleErrorDisplay() {
  // Show error blink for 2.4 seconds total (3 blinks)
  if (millis() - errorDisplayStart >= 2400) {
    Serial.println("Error display complete, returning to time display");
    currentState = STATE_TIME_DISPLAY;
    return;
  }

  // Blink ERR! with 400ms on/off
  unsigned long elapsed = millis() - errorDisplayStart;
  int cycle = elapsed / 400;
  if (cycle % 2 == 0) {
    display4Chars("ERR!");
  } else {
    clearDisplay();
  }
}

// ===== Display Functions =====

void clearFrame() {
  memset(frame, 0, sizeof(frame));
}

void clearDisplay() {
  clearFrame();
  renderFrame();
}

// ===== Time-of-day scene intro =====
// Plays at the start of every TIME visit (SCENE_INTRO_MS of the 7 s slot).
// The bucket comes from sceneForHour(); celestial motion is progress-driven
// (p = 0..1 across the intro) like the wifi fan build-up, while twinkles and
// pulses key off millis() so they keep moving between renders. Every scene
// shares the ground line on row 7.

SceneKind sceneForHour(int hours) {
  // The schedule is a flat 24-entry lookup generated from scene_config.json
  // (SCENE_HOUR_MAP in scene_config.h); night wraps midnight via the map.
  int8_t mapped = SCENE_HOUR_MAP[hours % 24];
  if (mapped < 0) return SCENE_NIGHT;   // defensive: schedule gap -> night
  return (SceneKind)mapped;
}

SceneKind pickScene() {
  if (SCENE_FORCE >= 6) return (SceneKind)(sceneVisit++ % 6);  // bench cycle
  if (SCENE_FORCE >= 0) return (SceneKind)SCENE_FORCE;         // force one
  return sceneForHour(timeClient.getHours());                  // follow clock
}

static void sceneGround() {
  for (int c = 0; c < 12; c++) frame[7][c] = 1;
}

// One twinkling star: lit 2 of every 3 steps, phase-offset per star.
static void sceneStar(int row, int col, int phase) {
  if (((millis() / 350) + phase) % 3 != 0) frame[row][col] = 1;
}

// 4x4 rounded sun disc at cols 4-7 (matrix center), clipped to rows 0-6 so
// it can rise from / sink behind the ground line cleanly.
static void sceneSun(int top) {
  static const uint8_t disc[4][4] = {
    {0,1,1,0},
    {1,1,1,1},
    {1,1,1,1},
    {0,1,1,0},
  };
  for (int r = 0; r < 4; r++) {
    int fr = top + r;
    if (fr < 0 || fr > 6) continue;
    for (int c = 0; c < 4; c++) {
      if (disc[r][c]) frame[fr][4 + c] = 1;
    }
  }
}

// 21:00-04:59 — crescent moon over the ground, four stars twinkling.
static void sceneNight() {
  sceneGround();
  static const uint8_t moon[5][5] = {   // lit on the left, opening right
    {0,1,1,0,0},
    {1,1,0,0,0},
    {1,1,0,0,0},
    {1,1,0,0,0},
    {0,1,1,0,0},
  };
  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < 5; c++) {
      if (moon[r][c]) frame[1 + r][7 + c] = 1;
    }
  }
  sceneStar(1, 2, 0);
  sceneStar(3, 4, 1);
  sceneStar(5, 1, 2);
  sceneStar(0, 4, 3);
}

// 05:00-06:59 — the sun climbs out of the ground while the stars wink out.
static void sceneSunrise(float p) {
  sceneGround();
  if (p < 0.45f) sceneStar(5, 1, 2);
  if (p < 0.60f) sceneStar(1, 2, 0);
  if (p < 0.75f) sceneStar(3, 4, 1);
  if (p < 0.90f) sceneStar(0, 4, 3);
  sceneSun(7 - (int)(p * 5.5f));   // top: 7 (hidden) -> 2 (rows 2-5)
}

// 07:00-10:59 — a small sun with pulsing rays and a cloud drifting by.
static void sceneMorning(float p) {
  sceneGround();
  for (int r = 1; r <= 3; r++) {         // 3x3 sun, upper-left
    for (int c = 1; c <= 3; c++) frame[r][c] = 1;
  }
  if ((millis() / 300) % 2 == 0) {       // rays pulse
    frame[0][2] = 1;
    frame[4][2] = 1;
    frame[2][0] = 1;
    frame[2][4] = 1;
  }
  static const uint8_t cloud[2][4] = {   // drifts left -> right at rows 4-5
    {0,1,1,0},
    {1,1,1,1},
  };
  int cx = (int)(p * 16.0f) - 4;
  for (int r = 0; r < 2; r++) {
    for (int c = 0; c < 4; c++) {
      int fc = cx + c;
      if (cloud[r][c] && fc >= 0 && fc < 12) frame[4 + r][fc] = 1;
    }
  }
}

// 11:00-16:59 — full sunburst: cardinal rays pulse, diagonals hold.
static void sceneDay() {
  sceneGround();
  sceneSun(1);                     // disc at rows 1-4, cols 4-7
  frame[0][4] = 1;                 // diagonal rays (always on)
  frame[0][7] = 1;
  frame[5][4] = 1;
  frame[5][7] = 1;
  if ((millis() / 300) % 2 == 0) {  // cardinal rays (pulsing)
    frame[0][5] = 1;
    frame[0][6] = 1;
    frame[5][5] = 1;
    frame[5][6] = 1;
    frame[2][3] = 1;
    frame[3][3] = 1;
    frame[2][8] = 1;
    frame[3][8] = 1;
  }
}

// 17:00-18:59 — the sun sinks behind the ground under thin cloud streaks.
static void sceneSunset(float p) {
  sceneGround();
  sceneSun(2 + (int)(p * 5.0f));   // top: 2 (rows 2-5) -> 7 (hidden)
  int drift = (int)(p * 3.0f);     // clouds creep left as the sun drops
  for (int c = 0; c < 4; c++) {
    int fc = c - drift;
    if (fc >= 0) frame[1][fc] = 1;
    fc = 8 + c - drift;
    if (fc < 12) frame[2][fc] = 1;
  }
}

// 19:00-20:59 — the afterglow dome shrinks back into the horizon while
// stars pop in one by one. Same dome as the static SCENE_ART still, in
// three stages so the glow reads as receding rather than blinking out.
static void sceneDusk(float p) {
  sceneGround();
  if (p < 0.35f) {                 // full dome on rows 4-6
    for (int c = 4; c <= 7; c++) frame[4][c] = 1;
    for (int c = 3; c <= 8; c++) frame[5][c] = 1;
    for (int c = 2; c <= 9; c++) frame[6][c] = 1;
  } else if (p < 0.65f) {          // dome sinks to rows 5-6
    for (int c = 3; c <= 8; c++) frame[5][c] = 1;
    for (int c = 2; c <= 9; c++) frame[6][c] = 1;
  } else if (p < 0.85f) {          // last band left on the horizon
    for (int c = 4; c <= 7; c++) frame[6][c] = 1;
  }
  if (p > 0.15f) sceneStar(1, 2, 0);
  if (p > 0.35f) sceneStar(3, 4, 1);
  if (p > 0.55f) sceneStar(5, 1, 2);
  if (p > 0.75f) sceneStar(0, 4, 3);
}

void displayScene(unsigned long elapsed) {
  float p = (float)elapsed / (float)SCENE_INTRO_MS;
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;
  clearFrame();
  switch (sceneKind) {
    case SCENE_NIGHT:   sceneNight();   break;
    case SCENE_SUNRISE: sceneSunrise(p); break;
    case SCENE_MORNING: sceneMorning(p); break;
    case SCENE_DAY:     sceneDay();     break;
    case SCENE_SUNSET:  sceneSunset(p);  break;
    case SCENE_DUSK:    sceneDusk(p);    break;
  }
  renderFrame();
}

// SCENE_STYLE=1: hold the purpose-drawn still for the whole intro window.
void displaySceneArt() {
  clearFrame();
  memcpy(frame, SCENE_ART[sceneKind], sizeof(frame));
  renderFrame();
}

void drawColon(bool visible, int col) {
  if (visible) {
    frame[3][col] = 1;
    frame[5][col] = 1;
  }
}

// Indicators live in the top-center pixels that no display phase ever uses
// (rows 0-1 are free for compact text; columns 5-6 are free for large digits)
void drawIndicators() {
  // Stale-time indicator: solid when there has been no sync for over an hour
  // (WiFi loss itself now triggers the full NO WIFI banner instead)
  if (timeValid && (millis() - lastNtpSync > STALE_SYNC_MS)) {
    frame[0][6] = 1;
  }
}

void renderCharsIntoFrame(const char* text) {
  for (int i = 0; i < 4 && text[i] != '\0'; i++) {
    renderCompactChar(text[i], i * 3);
  }
}

void display4Chars(const char* text) {
  clearFrame();
  renderCharsIntoFrame(text);
  renderFrame();
}

void displayTime(int hours, int minutes) {
  clearFrame();

  if (TIME_FORMAT_24H) {
    char timeStr[5];
    snprintf(timeStr, sizeof(timeStr), "%02d%02d", hours, minutes);
    renderCharsIntoFrame(timeStr);
    // No spare column for a colon in 4-digit mode: blink a tick pixel instead
    if ((millis() / 1000) % 2 == 0) frame[1][5] = 1;
  } else {
    bool isPM = (hours >= 12);
    int displayHour = hours % 12;
    if (displayHour == 0) displayHour = 12;

    char timeStr[5];
    if (displayHour >= 10) {
      timeStr[0] = '1';
      timeStr[1] = '0' + (displayHour % 10);
      timeStr[2] = ' ';
      timeStr[3] = isPM ? 'P' : 'A';
      timeStr[4] = '\0';
    } else {
      timeStr[0] = '0' + displayHour;
      timeStr[1] = ' ';
      timeStr[2] = isPM ? 'P' : 'A';
      timeStr[3] = 'M';
      timeStr[4] = '\0';
    }
    renderCharsIntoFrame(timeStr);
    // Colon goes in the blank cell after the hour (col 4 for "9 AM",
    // col 7 for "11 P"). Inside the second digit it would collide with
    // the '1' glyph and become invisible.
    drawColon(colonVisible, displayHour >= 10 ? 7 : 4);
  }

  drawIndicators();
  renderFrame();
}

void displayMinutesLarge(int minutes) {
  clearFrame();

  int tens = minutes / 10;
  int ones = minutes % 10;

  for (int row = 0; row < 7; row++) {
    for (int col = 0; col < 5; col++) {
      frame[row][col] = largeDigits[tens][row][col];
      frame[row][col + 7] = largeDigits[ones][row][col];
    }
  }

  drawIndicators();
  renderFrame();
}

// Date page, three stages driven by one entrance clock (set on each visit):
//   1. Fall: the month pair drops from above the top edge, one row per
//      DATE_DROP_MS, until it lands in the compact resting position.
//   2. Hold: the month alone for the step's hold_ms.
//   3. Flip: a 4-state cycle - month, '/', day, '/' - phase-anchored to the
//      end of the hold, so no flip state survives between visits. The slash
//      divider is the "you are here" marker: whatever follows a slash is the
//      day number, keeping the halves distinguishable even when equal
//      (11/11, 1/1, ...) where a plain 2-state flip draws identical pixels.
// The two pairs can't fit side by side at full size, so both halves reuse
// the same centered slots the year page uses.
void displayDate() {
  clearFrame();

  // getEpochTime() already includes the timezone/DST offset => local date
  int64_t days = (int64_t)timeClient.getEpochTime() / 86400;
  int y, m, d;
  epochToDate(days, y, m, d);

  const PhaseStep* step = stepFor(GEN_PHASE_DATE);
  unsigned long holdMs = step ? step->holdMs : 2000;
  unsigned long monthMs = step ? step->monthMs : 600;
  unsigned long slashMs = step ? step->slashMs : 300;
  unsigned long dayMs = step ? step->dayMs : 900;

  unsigned long el = millis() - dateEntranceStart;
  unsigned long fallMs = (unsigned long)(DATE_FALL_TOP + COMPACT_ROW_OFFSET) * DATE_DROP_MS;

  // Stage 1: the month pair falls in (negative offsets clip away above
  // the matrix, so the pair starts fully hidden and drops into view).
  if (el < fallMs) {
    int rowOffset = (int)(el / DATE_DROP_MS) - DATE_FALL_TOP;
    drawDatePair(m, rowOffset);
    drawIndicators();
    renderFrame();
    return;
  }

  // Stage 2: the month alone, holding for the step's hold_ms
  if (el < fallMs + holdMs) {
    drawDatePair(m, COMPACT_ROW_OFFSET);
    drawIndicators();
    renderFrame();
    return;
  }

  // Stage 3: the 4-state flip: month -> '/' -> day -> '/' -> (repeat)
  unsigned long cycle = monthMs + slashMs + dayMs + slashMs;
  unsigned long t = (el - fallMs - holdMs) % cycle;
  if (t < monthMs) {
    drawDatePair(m, COMPACT_ROW_OFFSET);         // month
  } else if (t < monthMs + slashMs) {
    drawSlash();                                  // divider
  } else if (t < monthMs + slashMs + dayMs) {
    drawDatePair(d, COMPACT_ROW_OFFSET);         // day
  } else {
    drawSlash();                                  // divider
  }
  drawIndicators();
  renderFrame();
}

// The flip divider: a '/' diagonal in the gap between the two digit slots
// (cols 4-7, rows 2-5; the indicator dots above at rows 0-1 don't collide).
// Two pixels thick so it reads clearly at 12x8.
void drawSlash() {
  static const uint8_t slash[8][12] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},
    {0,0,0,0,1,1,0,0,0,0,0,0},
    {0,0,0,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0}
  };
  memcpy(frame, slash, sizeof(frame));
}

// The two-digit date pair in its centered slots (cols 2 and 7, mirroring
// the year page) at a caller-set vertical offset for the drop-in entrance.
void drawDatePair(int value, int rowOffset) {
  stampCompactGlyph('0' + (value / 10) % 10, 2, rowOffset);
  stampCompactGlyph('0' + value % 10, 7, rowOffset);
}

void displayYear() {
  clearFrame();

  int64_t days = (int64_t)timeClient.getEpochTime() / 86400;
  int y, m, d;
  epochToDate(days, y, m, d);

  // Two-digit year with a 2-column gap, centered (mirrors the large
  // digits' spacing): '2' at cols 2-4, blank 5-6, '6' at cols 7-9
  char yearStr[3];
  snprintf(yearStr, sizeof(yearStr), "%02d", y % 100);
  renderCompactChar(yearStr[0], 2);
  renderCompactChar(yearStr[1], 7);

  drawIndicators();
  renderFrame();
}

// Day of the week (0 = Sunday) from the local epoch day count.
// 1970-01-01 was a Thursday, so (days + 4) % 7 lands on the right name.
int getDayOfWeek() {
  int64_t days = (int64_t)timeClient.getEpochTime() / 86400;
  return (int)((days + 4) % 7);
}

// Pack text into a column buffer: each byte is one column, bit r = row r.
// Large glyphs are 7x5 ('0'-'9' at indices 0-9, 'A'-'Z' at 10-35); a space
// widens the blank run instead of drawing a glyph. Any two glyphs are
// separated by 2 blank columns (the minutes-page convention), and each
// space adds 4 more. Returns the packed length.
int packMarquee(const char* text, uint8_t* buf, int bufSize) {
  int len = 0;
  int gap = 1;                      // 1 blank leads the first glyph
  bool drewGlyph = false;
  for (int i = 0; text[i] && len < bufSize; i++) {
    char c = text[i];
    if (c == ' ') {
      gap += 4;                     // spaces widen the blank run
      continue;
    }
    if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')) continue;
    if (drewGlyph) gap += 2;        // standard inter-glyph gap
    while (gap > 0 && len < bufSize) {
      buf[len++] = 0;
      gap--;
    }
    gap = 0;
    int idx = (c >= 'A' && c <= 'Z') ? 10 + (c - 'A') : (c - '0');
    for (int col = 0; col < 5 && len < bufSize; col++) {
      uint8_t b = 0;
      for (int row = 0; row < 7; row++) {
        if (largeChars[idx][row][col]) b |= (1 << row);   // rows 0-6
      }
      buf[len++] = b;
    }
    drewGlyph = true;
  }
  return len;
}

void buildDayScroll(const char* name) {
  scrollLen = 0;
  // 12 lead-in columns so the word enters cleanly from the right edge
  for (int i = 0; i < 12; i++) scrollBuf[scrollLen++] = 0;
  scrollLen += packMarquee(name, scrollBuf + scrollLen, (int)sizeof(scrollBuf) - scrollLen);
  // 12 trail-out columns so the word exits fully to the left
  for (int i = 0; i < 12 && scrollLen < (int)sizeof(scrollBuf); i++) scrollBuf[scrollLen++] = 0;
}

// Day-name marquee in LARGE letters: the day name scrolls in from the right
// edge, crosses the display, and exits left. At 90 ms/col, WEDNESDAY (the
// longest name, 73 ticks) takes ~6.6 s of the 7 s slot; shorter names finish
// earlier and the display just sits blank for the remainder of the phase.
void displayDayScroll() {
  const PhaseStep* step = stepFor(GEN_PHASE_DAY);
  unsigned long scrollMs = step ? step->scrollMs : 90;
  if (millis() - lastScrollTick >= scrollMs) {
    lastScrollTick = millis();
    if (scrollPos < scrollLen - 12) scrollPos++;   // stop once the word has fully exited
  }

  clearFrame();
  for (int c = 0; c < 12; c++) {
    int idx = scrollPos + c;
    if (idx >= scrollLen) break;
    uint8_t b = scrollBuf[idx];
    for (int r = 0; r < 8; r++) {
      if (b & (1 << r)) frame[r][c] = 1;
    }
  }

  drawIndicators();
  renderFrame();
}

// ===== WiFi signal page =====
// Signal quality -> how many fan steps the build-up animation reaches:
// dot only (1) .. full fan with all three arcs (4).
int wifiRssiSteps(int rssi) {
  if (rssi >= -60) return 4;
  if (rssi >= -70) return 3;
  if (rssi >= -80) return 2;
  return 1;
}

// The dBm readout layout: the minus is a 2-px bar hugging the left edge
// (cols 0-1, row 4) — the 3-wide dash glyph would fuse with the next
// digit's filled edge into one streak. After a 2-column gap the digits
// land in fixed cells with a 1-column space between them: bar 0-1,
// gap 2-3, first digit 4-6, gap 7, second digit 8-10 ("-57"). Readings
// of -100 or weaker drop the bar and center the digits instead — the
// value is definitionally negative on this page.
void renderRssiNumber(long value) {
  char buf[7];
  snprintf(buf, sizeof(buf), "%ld", value);
  bool neg = (buf[0] == '-');
  const char* digits = neg ? buf + 1 : buf;
  int ndig = strlen(digits);

  if (neg && ndig <= 2) {
    frame[4][0] = 1;                          // minus bar (glyph-middle row)
    frame[4][1] = 1;
    for (int i = 0; i < ndig; i++) {
      renderCompactChar(digits[i], 4 + i * 4); // fixed cells 4 and 8
    }
  } else {
    // Wide reading (<= -100 dBm): no room for bar + gap; center the
    // digits with the same 1-column spacing.
    int start = (12 - (ndig * 4 - 1)) / 2;
    if (start < 0) start = 0;
    for (int i = 0; i < ndig; i++) {
      renderCompactChar(digits[i], start + i * 4);
    }
  }
}

// The page plays the fan build-up (one arc per the step's bars_ms, capped by
// signal quality, then a full-icon hold), then the RSSI takes over for
// the rest of the slot without ever sitting still: it first counts up
// from -100 like a meter locking onto the signal, then alternates
// between holding the real reading and a quick fan re-pulse.
void displayWifiRssi() {
  clearFrame();

  const PhaseStep* cfg = stepFor(GEN_PHASE_WIFI);
  unsigned long barsMs = cfg ? cfg->barsMs : 350;
  unsigned long barsHoldMs = cfg ? cfg->barsHoldMs : 600;
  unsigned long countMs = cfg ? cfg->countMs : 900;
  unsigned long numMs = cfg ? cfg->numMs : 1600;
  unsigned long pulseMs = cfg ? cfg->pulseMs : 500;

  unsigned long elapsed = millis() - wifiPageStart;
  int cap = wifiRssiSteps((int)wifiPageRssi);
  unsigned long buildMs = (unsigned long)cap * barsMs + barsHoldMs;

  if (elapsed < buildMs) {
    int step = (int)(elapsed / barsMs);   // 0 .. cap-1
    if (step > cap - 1) step = cap - 1;         // hold the full icon before the number
    memcpy(frame, WIFI_FAN[step], sizeof(frame));
  } else {
    unsigned long t = elapsed - buildMs;

    if (t < countMs) {
      // Count-up: -100 (no signal) climbs to the real reading.
      long shown = -100L + (wifiPageRssi + 100L) * (long)t / (long)countMs;
      renderRssiNumber(shown);
    } else {
      unsigned long seg = (t - countMs) % (numMs + pulseMs);
      if (seg < numMs) {
        renderRssiNumber(wifiPageRssi);        // hold the real reading
      } else {
        // Quick fan re-pulse: a compressed rebuild between number holds.
        int pstep = (int)((seg - numMs) / (pulseMs / 4));
        if (pstep > 3) pstep = 3;
        if (pstep > cap - 1) pstep = cap - 1;
        memcpy(frame, WIFI_FAN[pstep], sizeof(frame));
      }
    }
  }

  drawIndicators();
  renderFrame();
}

// ===== NO WIFI banner (takes over while WiFi is down) =====
// X flashes ~2 s to grab attention, then "NO WIFI" marquees past in large
// letters and loops until the connection returns. Fully non-blocking so
// serviceWifi()/OTA keep running underneath.
void startWarnBanner() {
  warnActive = true;
  sweepActive = false;            // banner overrides any in-flight transition
  warnStage = WARN_FLASH;
  warnStageStart = millis();
  warnLastTick = 0;               // first scroll step fires immediately

  // Same 12-column lead-in / trail-out padding as the day page; the trail
  // doubles as the pause between repeats when the loop wraps.
  warnLen = 0;
  for (int i = 0; i < 12 && warnLen < (int)sizeof(warnBuf); i++) warnBuf[warnLen++] = 0;
  warnLen += packMarquee("NO WIFI", warnBuf + warnLen, (int)sizeof(warnBuf) - warnLen);
  for (int i = 0; i < 12 && warnLen < (int)sizeof(warnBuf); i++) warnBuf[warnLen++] = 0;
}

void displayNoWifiWarning() {
  unsigned long now = millis();

  if (warnStage == WARN_FLASH) {
    unsigned long el = now - warnStageStart;
    unsigned long flashPeriod = WARN_X_ON_MS + WARN_X_OFF_MS;
    if (el / flashPeriod >= (unsigned long)WARN_X_FLASHES) {
      warnStage = WARN_SCROLL;
      warnStageStart = now;
      warnPos = 0;
    } else {
      clearFrame();
      if ((el % flashPeriod) < WARN_X_ON_MS) {
        memcpy(frame, XMARK, sizeof(frame));
      }
      renderFrame();
      return;
    }
  }

  // WARN_SCROLL: marquee window with wrap-around for a continuous loop
  if (warnLen <= 0) return;
  if (now - warnLastTick >= WARN_SCROLL_MS) {
    warnLastTick = now;
    warnPos = (warnPos + 1) % warnLen;
  }

  clearFrame();
  for (int c = 0; c < 12; c++) {
    uint8_t b = warnBuf[(warnPos + c) % warnLen];
    for (int r = 0; r < 8; r++) {
      if (b & (1 << r)) frame[r][c] = 1;
    }
  }
  renderFrame();
}

// ===== Two-eyed face (intermittent) =====
// Two "0" glyphs (largeDigits[0]) at the minutes-page positions:
// left eye cols 0-4, right eye cols 7-11, rows 0-6. Indicator pixels
// ([0][5],[0][6],[1][5]) live in the gap between the eyes.
// Pupil: 1-wide x 2-tall vertical pill inside each eye's 3-wide interior;
// center rest, look left/right, then a blink.
void drawEyes() {
  for (int r = 0; r < 7; r++) {
    for (int c = 0; c < 5; c++) {
      frame[r][c] = largeDigits[0][r][c];       // left eye
      frame[r][c + 7] = largeDigits[0][r][c];   // right eye
    }
  }
}

void drawEyePupils(int pose) {
  // pupil col offset within each eye's 3-wide interior (cols 1-3)
  int pcol;
  switch (pose) {
    case EYE_LEFT:   pcol = 1; break;
    case EYE_RIGHT:  pcol = 3; break;
    default:         pcol = 2; break;   // CENTER
  }
  for (int r = 3; r <= 4; r++) {
    frame[r][pcol] = 1;       // left eye
    frame[r][pcol + 7] = 1;   // right eye
  }
}

void drawEyeLids() {
  // Blink: horizontal bar across the middle of each eye
  for (int r = 3; r <= 4; r++) {
    for (int c = 1; c <= 3; c++) {
      frame[r][c] = 1;       // left eye
      frame[r][c + 7] = 1;   // right eye
    }
  }
}

void displayEye() {
  const PhaseStep* cfg = stepFor(GEN_PHASE_EYE);
  unsigned long poseMs = cfg ? cfg->stepMs : 700;
  unsigned long blinkMs = cfg ? cfg->blinkMs : 400;
  unsigned long stepMs = (EYE_SEQ[eyeStep] == EYE_BLINK) ? blinkMs : poseMs;
  if (millis() - lastEyeStep >= stepMs) {
    lastEyeStep = millis();
    eyeStep++;
    if (eyeStep >= EYE_SEQ_LEN) eyeStep = 0;
  }

  clearFrame();

  drawEyes();
  if (EYE_SEQ[eyeStep] == EYE_BLINK) {
    drawEyeLids();
  } else {
    drawEyePupils(EYE_SEQ[eyeStep]);
  }

  drawIndicators();
  renderFrame();
}

// Look up a compact 5x3 glyph and stamp it at startCol with a caller-set
// vertical offset (compact glyphs rest at rows 2-6, i.e. offset 2). Rows
// and columns outside the frame are clipped — the date page's drop-in
// entrance uses negative offsets to start the pair above the matrix.
void stampCompactGlyph(char c, int startCol, int rowOffset) {
  c = toupper(c);

  const uint8_t (*pattern)[3] = nullptr;

  if (c >= '0' && c <= '9') {
    pattern = compactDigits[c - '0'];
  } else if (c >= 'A' && c <= 'Z') {
    pattern = compactAlpha[c - 'A'];
  } else if (c == ' ') {
    pattern = (const uint8_t (*)[3])compactSpace;
  } else if (c == ':') {
    pattern = (const uint8_t (*)[3])compactColon;
  } else if (c == '-') {
    pattern = (const uint8_t (*)[3])compactDash;
  } else if (c == '!') {
    pattern = (const uint8_t (*)[3])compactExclamation;
  } else {
    pattern = (const uint8_t (*)[3])compactSpace;
  }

  for (int row = 0; row < 5; row++) {
    for (int col = 0; col < 3; col++) {
      if (startCol + col >= 0 && startCol + col < 12 &&
          row + rowOffset >= 0 && row + rowOffset < 8) {
        frame[row + rowOffset][startCol + col] = pattern[row][col];
      }
    }
  }
}

void renderCompactChar(char c, int startCol) {
  stampCompactGlyph(c, startCol, COMPACT_ROW_OFFSET);
}

void showWiFiStatus(bool connected) {
  clearDisplay();
  memcpy(frame, connected ? CHECKMARK : XMARK, sizeof(frame));
  renderFrame();
}

void displayLoadingAnimation() {
  const char* loadMessages[] = {"LOAD", "OAD.", "AD..", "D..."};
  for (int i = 0; i < 4; i++) {
    display4Chars(loadMessages[i]);
    delay(300);
  }
}

#if CONFIG_BOOT_LOGO_ENABLED
// ===== Boot logo splash =====
// Plays the flipbook once at its own frame durations, holds a minimum
// total (CONFIG_BOOT_LOGO_HOLD_MS) so a single-frame logo is actually
// visible, then keeps looping until the WiFi gets the IP — the logo fills
// the radio connect wait. Blocking by design: setup() is single-threaded
// and the loop() state machine starts after this.
void displayBootLogo() {
  unsigned long bootStart = millis();
  unsigned long elapsed = 0;
  unsigned int frameIdx = 0;
  unsigned long frameStart = millis();
  while (true) {
    // unpack the current frame: 3 words, row-major MSB-first
    for (int r = 0; r < 8; r++) {
      for (int c = 0; c < 12; c++) {
        uint32_t bit = (uint32_t)r * 12 + c;
        uint32_t w = bit / 32;
        uint32_t sh = 31 - (bit % 32);
        frame[r][c] = (CONFIG_BOOT_LOGO_FRAMES[frameIdx][w] >> sh) & 1u;
      }
    }
    matrix.renderBitmap(frame, 8, 12);

    if (millis() - frameStart >= CONFIG_BOOT_LOGO_FRAME_MS[frameIdx]) {
      frameIdx = (frameIdx + 1) % CONFIG_BOOT_LOGO_FRAME_COUNT;
      frameStart = millis();
    }
    elapsed = millis() - bootStart;

    // IP acquired and the minimum hold elapsed: splash done
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0) &&
        elapsed >= CONFIG_BOOT_LOGO_HOLD_MS) {
      break;
    }
    // connect timeout: fall through to the normal boot path
    if (elapsed >= WIFI_CONNECT_TIMEOUT) {
      break;
    }
    delay(10);
  }
}
#endif
