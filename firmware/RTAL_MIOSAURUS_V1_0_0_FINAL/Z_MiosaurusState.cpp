#include "Z_MiosaurusState.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/portmacro.h>
#include <math.h>

namespace {

struct StateCommand {
  uint8_t id;
  float value;
  uint8_t normalized; // 1: value 0..1; 0: actual z33 value
};

struct StateRuntime {
  float value;
  float target;
};

// Exact ranges/defaults from the legacy MIOSAURUS source.
static const MioParamStateDef kDefs[PARAM_COUNT] = {
  {"OSC1 WAV", 22, 0.00f,  0.00f, 1.00f, false},
  {"OSC1 LVL", 89, 0.85f,  0.00f, 1.00f, false},
  {"OSC1 PW",  23, 0.50f,  0.05f, 0.95f, false},
  {"OSC1 PWM", 30, 0.00f,  0.00f, 1.00f, false},
  {"OSC2 WAV", 26, 0.00f,  0.00f, 1.00f, false},
  {"OSC2 LVL", 90, 0.80f,  0.00f, 1.00f, false},
  {"OSC2 PW",  27, 0.50f,  0.05f, 0.95f, false},
  {"OSC2 PWM", 31, 0.00f,  0.00f, 1.00f, false},
  {"DETUNE",   77,-0.08f, -12.0f,12.0f, false},
  {"SUB LVL",  52, 0.35f,  0.00f, 1.00f, false},
  {"CUTOFF",  102, 0.50f,  0.00f, 1.00f, false},
  {"RESO",    103, 0.14f,  0.00f, 1.00f, false},
  {"DRIVE",   105, 1.00f,  0.50f, 4.00f, false},
  {"F ENV",   106, 0.55f, -1.00f, 1.00f, false},
  {"KEYTRK",  115, 0.35f,  0.00f, 1.00f, false},
  {"VEL FILT", 86, 0.40f,  0.00f, 1.00f, false},
  {"F ATT",   109, 0.002f, 0.001f,3.00f, false},
  {"F DEC",   110, 0.16f,  0.001f,3.00f, false},
  {"F SUS",   111, 0.00f,  0.00f, 1.00f, false},
  {"F REL",   112, 0.18f,  0.001f,3.00f, false},
  {"A ATT",   118, 0.01f,  0.001f,2.00f, false},
  {"A DEC",   119, 0.20f,  0.001f,3.00f, false},
  {"A SUS",    75, 0.80f,  0.00f, 1.00f, false},
  {"A REL",    76, 0.18f,  0.001f,3.00f, false},
  {"LFO RATE", 28, 0.15f,  0.00f, 1.00f, false},
  {"LFO CTF",  29, 0.00f, -1.00f, 1.00f, false},
  {"LFO SYNC", 20, 0.00f,  0.00f, 1.00f, true },
  {"LFO DIV",  24, 0.30f,  0.00f, 1.00f, false},
  {"PB RNG",   14, 2.00f,  0.00f,12.00f, false},
  {"DLY VOL",  33, 0.15f,  0.00f, 1.00f, false},
  {"DLY FB",   34, 0.35f,  0.00f, 0.95f, false},
  {"DLY SYNC", 35, 1.00f,  0.00f, 1.00f, true },
  {"DLY DIV",  36, 0.40f,  0.00f, 1.00f, false},
  {"DLY TIME", 37, 0.25f,  0.00f, 1.00f, false},
  {"DLY DAMP", 38, 0.45f,  0.00f, 1.00f, false},
  {"FLG VOL", 116, 0.00f,  0.00f, 1.00f, false},
  {"FLG RATE",117, 0.18f,  0.00f, 1.00f, false},
  {"FLG DEPTH",79, 0.35f,  0.00f, 1.00f, false},
  {"FLG FB",   80, 0.12f,  0.00f, 0.85f, false},
  {"FLG MAN",  81, 0.22f,  0.00f, 1.00f, false},
  {"FLG POL",  82, 0.00f,  0.00f, 1.00f, true },
  {"FLG WID",  83, 0.25f,  0.00f, 1.00f, false},
  {"GLIDE",    78, 0.00f,  0.00f, 1.00f, false},
  {"VOL",       7, 0.70f,  0.00f, 1.00f, false},
  // V0.14B FIX1 LIGHT REVERB. Bank-1 defaults requested for MIOSAURUS.
  // These CC numbers may also exist in Bank 0; the bank selector keeps them independent.
  {"REV VOL",   118, 0.00f,  0.00f, 1.00f, false},
  {"REV SIZE",  119, 0.45f,  0.00f, 1.00f, false},
  {"REV DECAY",  75, 0.42f,  0.00f, 1.00f, false},
  {"REV DAMP",   76, 0.50f,  0.00f, 1.00f, false}
};

static StateRuntime sState[PARAM_COUNT];
static QueueHandle_t sCommandQueue = nullptr;
static portMUX_TYPE sStateMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint64_t sGuiDirtyMask = 0;

static inline float clampfLocal(float v, float lo, float hi) {
  if (!isfinite(v)) return lo;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static float normToActual(MioParamId id, float n) {
  const MioParamStateDef &d = kDefs[id];
  n = clampfLocal(n, 0.0f, 1.0f);
  if (d.discrete) return n >= 0.5f ? d.maxValue : d.minValue;

  // FIX9: larger 0.0st neutral zone for OSC2 DETUNE.
  // 46..54% of slider/MIDI travel is exactly zero.
  if (id == P_OSC2_DETUNE) {
    constexpr float lo = 0.46f;
    constexpr float hi = 0.54f;
    if (n >= lo && n <= hi) return 0.0f;
    if (n < lo) return -12.0f + (n / lo) * 12.0f;
    return ((n - hi) / (1.0f - hi)) * 12.0f;
  }

  return d.minValue + n * (d.maxValue - d.minValue);
}

static float actualToNorm(MioParamId id, float v) {
  const MioParamStateDef &d = kDefs[id];
  v = clampfLocal(v, d.minValue, d.maxValue);
  if (d.discrete) return v >= (d.minValue + d.maxValue) * 0.5f ? 1.0f : 0.0f;

  if (id == P_OSC2_DETUNE) {
    constexpr float lo = 0.46f;
    constexpr float hi = 0.54f;
    if (fabsf(v) < 0.0001f) return 0.5f;
    if (v < 0.0f) return ((v + 12.0f) / 12.0f) * lo;
    return hi + (v / 12.0f) * (1.0f - hi);
  }

  const float range = d.maxValue - d.minValue;
  if (range <= 0.0f) return 0.0f;
  return (v - d.minValue) / range;
}

static void applyCommand(const StateCommand &cmd) {
  if (cmd.id >= PARAM_COUNT) return;
  MioParamId id = (MioParamId)cmd.id;
  const MioParamStateDef &d = kDefs[id];
  float actual = cmd.normalized ? normToActual(id, cmd.value)
                                : clampfLocal(cmd.value, d.minValue, d.maxValue);
  if (d.discrete) actual = actual >= (d.minValue + d.maxValue) * 0.5f ? d.maxValue : d.minValue;

  portENTER_CRITICAL(&sStateMux);
  sState[id].target = actual;
  // V0.8 state bridge has no DSP smoother yet, so value follows target immediately.
  sState[id].value = actual;
  sGuiDirtyMask |= (1ULL << (uint8_t)id);
  portEXIT_CRITICAL(&sStateMux);
}

static bool queueValue(MioParamId id, float value, bool normalized) {
  if (!sCommandQueue || id >= PARAM_COUNT) return false;
  StateCommand cmd{(uint8_t)id, value, (uint8_t)(normalized ? 1 : 0)};
  return xQueueSend(sCommandQueue, &cmd, 0) == pdTRUE;
}

} // namespace

bool miosaurusStateBegin() {
  if (!sCommandQueue) sCommandQueue = xQueueCreate(32, sizeof(StateCommand));
  if (!sCommandQueue) return false;

  portENTER_CRITICAL(&sStateMux);
  sGuiDirtyMask = 0;
  for (uint8_t i = 0; i < PARAM_COUNT; ++i) {
    sState[i].value = kDefs[i].defaultValue;
    sState[i].target = kDefs[i].defaultValue;
    sGuiDirtyMask |= (1ULL << i);
  }
  portEXIT_CRITICAL(&sStateMux);
  return true;
}

void miosaurusStateControlService() {
  if (!sCommandQueue) return;
  StateCommand cmd;
  while (xQueueReceive(sCommandQueue, &cmd, 0) == pdTRUE) applyCommand(cmd);
}

bool miosaurusStateQueueGuiNormalized(MioParamId id, float normalized01) {
  return queueValue(id, normalized01, true);
}

bool miosaurusStateQueueActual(MioParamId id, float actualValue) {
  return queueValue(id, actualValue, false);
}

bool miosaurusStateQueueNormalized(MioParamId id, float normalized01) {
  return queueValue(id, normalized01, true);
}

float miosaurusStateGetActual(MioParamId id) {
  if (id >= PARAM_COUNT) return 0.0f;
  portENTER_CRITICAL(&sStateMux);
  float v = sState[id].value;
  portEXIT_CRITICAL(&sStateMux);
  return v;
}

float miosaurusStateGetTargetActual(MioParamId id) {
  if (id >= PARAM_COUNT) return 0.0f;
  portENTER_CRITICAL(&sStateMux);
  float v = sState[id].target;
  portEXIT_CRITICAL(&sStateMux);
  return v;
}

float miosaurusStateGetNormalized(MioParamId id) {
  return miosaurusStateNormalizedFromActual(id, miosaurusStateGetTargetActual(id));
}

float miosaurusStateActualFromNormalized(MioParamId id, float normalized01) {
  if (id >= PARAM_COUNT) return 0.0f;
  return normToActual(id, normalized01);
}

float miosaurusStateNormalizedFromActual(MioParamId id, float actualValue) {
  if (id >= PARAM_COUNT) return 0.0f;
  return actualToNorm(id, actualValue);
}

const MioParamStateDef &miosaurusStateDef(MioParamId id) {
  if (id >= PARAM_COUNT) id = P_OSC1_WAVE;
  return kDefs[id];
}

bool miosaurusStateConsumeGuiDirty(MioParamId &id, float &normalized01) {
  uint8_t found = 0xFF;
  float actual = 0.0f;
  portENTER_CRITICAL(&sStateMux);
  uint64_t mask = sGuiDirtyMask;
  if (mask) {
    for (uint8_t i = 0; i < PARAM_COUNT; ++i) {
      uint64_t bit = 1ULL << i;
      if (mask & bit) {
        found = i;
        sGuiDirtyMask &= ~bit;
        actual = sState[i].target;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&sStateMux);
  if (found == 0xFF) return false;
  id = (MioParamId)found;
  normalized01 = actualToNorm(id, actual);
  return true;
}

void miosaurusStateMarkAllGuiDirty() {
  portENTER_CRITICAL(&sStateMux);
  for (uint8_t i = 0; i < PARAM_COUNT; ++i) sGuiDirtyMask |= (1ULL << i);
  portEXIT_CRITICAL(&sStateMux);
}


void miosaurusStateSnapshotActual(float *outValues, size_t count) {
  if (!outValues) return;
  if (count > PARAM_COUNT) count = PARAM_COUNT;

  portENTER_CRITICAL(&sStateMux);
  for (size_t i = 0; i < count; ++i) {
    outValues[i] = sState[i].target;
  }
  portEXIT_CRITICAL(&sStateMux);
}

bool miosaurusStateApplyPresetActual(const float *values, size_t count) {
  if (!values || count != PARAM_COUNT) return false;

  portENTER_CRITICAL(&sStateMux);
  for (uint8_t i = 0; i < PARAM_COUNT; ++i) {
    const MioParamStateDef &d = kDefs[i];
    float actual = values[i];
    if (!isfinite(actual)) actual = d.defaultValue;
    if (actual < d.minValue) actual = d.minValue;
    if (actual > d.maxValue) actual = d.maxValue;
    if (d.discrete) {
      actual = actual >= (d.minValue + d.maxValue) * 0.5f
             ? d.maxValue : d.minValue;
    }
    sState[i].target = actual;
    sState[i].value = actual;
    sGuiDirtyMask |= (1ULL << i);
  }
  portEXIT_CRITICAL(&sStateMux);
  return true;
}
