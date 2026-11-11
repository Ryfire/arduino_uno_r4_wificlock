// GENERATED FILE — do not edit.
// Source of truth: scene_config.json (repo root). Edit it by hand or via
// tools/studio/index.html; `pio run` regenerates this file automatically
// (tools/generate_scene_config.py runs as a PlatformIO pre-build script).
#pragma once

#include <Arduino.h>

// Display phase ids; order must match the DisplayPhase enum in src/main.cpp.
enum GeneratedPhase : uint8_t {
  GEN_PHASE_TIME,
  GEN_PHASE_MINUTES,
  GEN_PHASE_DATE,
  GEN_PHASE_YEAR,
  GEN_PHASE_DAY,
  GEN_PHASE_WIFI,
  GEN_PHASE_EYE,
};

// One step of the display rotation: what shows, for how long, whether it
// plays at all. everyNCycles is only used by the eye phase (0 = never).
struct PhaseStep {
  uint8_t phase;
  unsigned long durationMs;
  bool enabled;
  uint8_t everyNCycles;
};

const PhaseStep SCENE_SEQUENCE[] = {
  { GEN_PHASE_TIME,       7000, true, 0 },   // time
  { GEN_PHASE_MINUTES,    7000, true, 0 },   // minutes
  { GEN_PHASE_DATE,       7000, true, 0 },   // date
  { GEN_PHASE_YEAR,       7000, true, 0 },   // year
  { GEN_PHASE_DAY,        7000, true, 0 },   // day
  { GEN_PHASE_WIFI,       7000, true, 0 },   // wifi
  { GEN_PHASE_EYE,        7000, true, 5 },   // eye
};
const uint8_t SCENE_SEQUENCE_LEN = sizeof(SCENE_SEQUENCE) / sizeof(SCENE_SEQUENCE[0]);

// Scene intro length within the TIME step (ms).
const unsigned long SCENE_INTRO_MS = 2500;
// Default scene flavor; a SCENE_STYLE define in secrets.h still wins.
#ifndef CONFIG_SCENE_STYLE_DEFAULT
#define CONFIG_SCENE_STYLE_DEFAULT 2
#endif

// hour (0-23) -> scene index: 0=night 1=sunrise 2=morning 3=day 4=sunset 5=dusk.
const int8_t SCENE_HOUR_MAP[24] = {
  0, 0, 0, 0, 0, 1, 1, 2, 2, 2, 2, 3,
  3, 3, 3, 3, 3, 4, 4, 5, 5, 0, 0, 0,
};
