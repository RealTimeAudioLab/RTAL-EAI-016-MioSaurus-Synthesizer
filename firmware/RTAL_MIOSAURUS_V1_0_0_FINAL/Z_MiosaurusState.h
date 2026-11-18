#pragma once
#include <Arduino.h>
#include "Z_MiosaurusGui.h"

// MIOSAURUS z33 parameter/state bridge.
// The actual ranges/defaults are copied from the hardware-confirmed z33 source.

struct MioParamStateDef {
  const char *name;
  uint8_t defaultCc;
  float defaultValue;
  float minValue;
  float maxValue;
  bool discrete;
};

bool miosaurusStateBegin();
void miosaurusStateControlService();

// GUI -> state. Non-blocking; safe to call from LVGL/Core0 GUI task.
bool miosaurusStateQueueGuiNormalized(MioParamId id, float normalized01);

// Future MIDI/preset/control path -> state, using the real z33 engineering value.
// These functions never call LVGL.
bool miosaurusStateQueueActual(MioParamId id, float actualValue);
bool miosaurusStateQueueNormalized(MioParamId id, float normalized01);

float miosaurusStateGetActual(MioParamId id);
float miosaurusStateGetTargetActual(MioParamId id);
float miosaurusStateGetNormalized(MioParamId id);
float miosaurusStateActualFromNormalized(MioParamId id, float normalized01);
float miosaurusStateNormalizedFromActual(MioParamId id, float actualValue);
const MioParamStateDef &miosaurusStateDef(MioParamId id);

// GUI task polls dirty values and updates LVGL there, never from control/audio tasks.
bool miosaurusStateConsumeGuiDirty(MioParamId &id, float &normalized01);
void miosaurusStateMarkAllGuiDirty();


// Preset snapshot/apply path. This bypasses the small real-time command queue
// intentionally, so all PARAM_COUNT values can be restored atomically.
void miosaurusStateSnapshotActual(float *outValues, size_t count);
bool miosaurusStateApplyPresetActual(const float *values, size_t count);
