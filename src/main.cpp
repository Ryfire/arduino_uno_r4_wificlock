/*
 * UNO R4 WiFi - Smart Digital Clock with 4-Character Display
 *
 * Features:
 * - NTP-synced real time with automatic US daylight-saving changes
 * - Non-blocking WiFi reconnect: retries forever with backoff
 * - NTP retry backoff + server rotation; clock keeps running offline
 * - Display cycle: time (7s) -> large minutes (7s) -> date (7s) -> year (7s)
 *   -> day-name scroll (7s) -> wifi signal (7s); a periodic eyeball
 *   animation every Nth cycle
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
const unsigned long PHASE_INTERVAL = 7000;      // seconds per display phase
const unsigned long DATE_FLIP_MS = 350;         // month <-> day swap on date page
const unsigned long DATE_DROP_MS = 70;          // date entrance: ms per fall row
const unsigned long DATE_HOLD_MS = 3000;        // date entrance: month alone before flipping
const int DATE_FALL_TOP = 5;                    // date entrance: fall starts this many rows above the matrix
const int COMPACT_ROW_OFFSET = 2;               // compact glyphs rest at frame rows 2-6
const unsigned long DAY_SCROLL_MS = 90;         // day-name scroll: ms per column
const unsigned long SWEEP_MS = 40;              // phase transition: ms per column of the wipe
const unsigned long EYE_STEP_MS = 700;          // eye animation: ms per pose
const unsigned long EYE_BLINK_MS = 400;         // eye animation: closed-lid duration
const unsigned long WIFI_BARS_MS = 350;         // wifi page: ms per fan-buildup step
const unsigned long WIFI_BARS_HOLD = 600;       // wifi page: full-icon hold before the number
const unsigned long WARN_X_ON_MS = 450;         // banner: X visible per flash
const unsigned long WARN_X_OFF_MS = 250;        // banner: dark gap between flashes
const int WARN_X_FLASHES = 3;                   // banner: flashes before the marquee
const unsigned long WARN_SCROLL_MS = 90;        // banner: ms per marquee column
const int EYE_EVERY_N_CYCLES = 5;               // eye plays every Nth cycle (0 = off)
const unsigned long WIFI_CONNECT_TIMEOUT = 15000; // per-attempt timeout (ms)

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

// ===== Day-name scroll state =====
uint8_t scrollBuf[96];            // packed columns: bit r = row r (fits WEDNESDAY: 85 cols)
int scrollLen = 0;                // total columns in buffer
int scrollPos = 0;                // current window offset
unsigned long lastScrollTick = 0;

// ===== Eye animation state =====
enum EyePose { EYE_CENTER, EYE_LEFT, EYE_RIGHT, EYE_BLINK };
// Two eyes: center rest, look left/right, then blink once. The show
// loops for the whole 7 s phase every EYE_EVERY_N_CYCLES-th cycle.
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
void stampCompactGlyph(char c, int startCol, int rowOffset);
void drawDatePair(int value, int rowOffset);
int getDayOfWeek();
void buildDayScroll(const char* name);
void clearDisplay();
void drawColon(bool visible, int col);
void drawIndicators();
void renderCompactChar(char c, int startCol);
void showWiFiStatus(bool connected);
void displayLoadingAnimation();

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
  Serial.println("Connecting to WiFi...");
  displayLoadingAnimation();

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
void advancePhase(int phases) {
  if (millis() - lastPhaseToggle >= PHASE_INTERVAL) {
    lastPhaseToggle = millis();
    DisplayPhase next = (DisplayPhase)((displayPhase + 1) % phases);

    // The eye is a periodic page: only run it every Nth cycle. The count
    // still increments on the skip, so "every 5th cycle" holds.
    if (next == PHASE_EYE) {
      cycleCount++;
      if (EYE_EVERY_N_CYCLES > 0 && cycleCount % EYE_EVERY_N_CYCLES != 0) {
        next = PHASE_TIME;
      }
    }

    if (next != displayPhase) {
      // Day-scroll marquees itself in; the date page falls in on its own
      // (see displayDate), so neither of those uses the column wipe.
      if (next != PHASE_DAY && next != PHASE_DATE) {
        startSweep();
      }
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
    }
  }
}

void renderDisplayPhase() {
  advancePhase(7);

  int hours = timeClient.getHours();    // timezone/DST offset applied
  int minutes = timeClient.getMinutes();

  switch (displayPhase) {
    case PHASE_TIME:
      if (millis() - lastBlinkTime >= 1000) {
        colonVisible = !colonVisible;
        lastBlinkTime = millis();
      }
      displayTime(hours, minutes);
      break;
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
//   2. Hold: the month alone for DATE_HOLD_MS.
//   3. Flip: month <-> day every DATE_FLIP_MS, phase-anchored to the end
//      of the hold, so no flip state survives between visits.
// The two pairs can't fit side by side at full size, so both halves reuse
// the same centered slots the year page uses.
void displayDate() {
  clearFrame();

  // getEpochTime() already includes the timezone/DST offset => local date
  int64_t days = (int64_t)timeClient.getEpochTime() / 86400;
  int y, m, d;
  epochToDate(days, y, m, d);

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

  // Stage 2: the month alone, holding for DATE_HOLD_MS
  if (el < fallMs + DATE_HOLD_MS) {
    drawDatePair(m, COMPACT_ROW_OFFSET);
    drawIndicators();
    renderFrame();
    return;
  }

  // Stage 3: the regular month <-> day flip
  bool showMonth = ((el - fallMs - DATE_HOLD_MS) / DATE_FLIP_MS) % 2 == 0;
  drawDatePair(showMonth ? m : d, COMPACT_ROW_OFFSET);
  drawIndicators();
  renderFrame();
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
  if (millis() - lastScrollTick >= DAY_SCROLL_MS) {
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

// The page plays the fan build-up (one arc per WIFI_BARS_MS, capped by
// signal quality, then a full-icon hold), then shows the RSSI snapshot
// for the rest of the 7 s slot.
void displayWifiRssi() {
  clearFrame();

  unsigned long elapsed = millis() - wifiPageStart;
  int cap = wifiRssiSteps((int)wifiPageRssi);
  unsigned long buildMs = (unsigned long)cap * WIFI_BARS_MS + WIFI_BARS_HOLD;

  if (elapsed < buildMs) {
    int step = (int)(elapsed / WIFI_BARS_MS);   // 0 .. cap-1
    if (step > cap - 1) step = cap - 1;         // hold the full icon before the number
    memcpy(frame, WIFI_FAN[step], sizeof(frame));
  } else {
    // Hand-placed readout: the minus is a 2-px bar hugging the left edge
    // (cols 0-1, row 4) — the 3-wide dash glyph would fuse with the next
    // digit's filled edge into one streak. After a 2-column gap the digits
    // land in fixed cells with a 1-column space between them: bar 0-1,
    // gap 2-3, first digit 4-6, gap 7, second digit 8-10 ("-57").
    char buf[7];
    snprintf(buf, sizeof(buf), "%ld", wifiPageRssi);
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
      // digits with the same 1-column spacing — the value is
      // definitionally negative on this page.
      int start = (12 - (ndig * 4 - 1)) / 2;
      if (start < 0) start = 0;
      for (int i = 0; i < ndig; i++) {
        renderCompactChar(digits[i], start + i * 4);
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
  unsigned long stepMs = (EYE_SEQ[eyeStep] == EYE_BLINK) ? EYE_BLINK_MS : EYE_STEP_MS;
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
