#ifndef SECRETS_H
#define SECRETS_H

// ===== WiFi credentials =====
const char* WIFI_SSID = "WIFI";
const char* WIFI_PASSWORD = "password";

// ===== Time configuration (optional - main.cpp has built-in defaults) =====
// Standard-time offset from UTC in seconds:
//   EST = -5*3600, CST = -6*3600, MST = -7*3600, PST = -8*3600, UTC = 0
#define TZ_BASE_OFFSET (-5 * 3600)

// 1 = automatic US daylight saving (2nd Sun Mar -> 1st Sun Nov)
// 0 = fixed offset, no DST
#define TZ_USE_US_DST 1

// 1 = 24-hour display (e.g. 1435), 0 = 12-hour with AM/PM (e.g. "2 PM")
#define TIME_FORMAT_24H 0

#endif
