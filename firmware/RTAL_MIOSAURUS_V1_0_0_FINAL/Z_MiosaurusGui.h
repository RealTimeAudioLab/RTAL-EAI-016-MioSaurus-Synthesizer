#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Exact numeric order of MIOSAURUS z33 ParamId.
enum MioParamId : uint8_t {
  P_OSC1_WAVE = 0,
  P_OSC1_LEVEL,
  P_OSC1_PW,
  P_OSC1_PWM_AMT,
  P_OSC2_WAVE,
  P_OSC2_LEVEL,
  P_OSC2_PW,
  P_OSC2_PWM_AMT,
  P_OSC2_DETUNE,
  P_SUB_LEVEL,
  P_FILTER_CUTOFF,
  P_FILTER_RESO,
  P_FILTER_DRIVE,
  P_FILTER_ENV_AMT,
  P_FILTER_KEYTRACK,
  P_FILTER_VEL_AMT,
  P_FILTER_ATTACK,
  P_FILTER_DECAY,
  P_FILTER_SUSTAIN,
  P_FILTER_RELEASE,
  P_AMP_ATTACK,
  P_AMP_DECAY,
  P_AMP_SUSTAIN,
  P_AMP_RELEASE,
  P_LFO_RATE,
  P_LFO_FILTER_AMT,
  P_LFO_SYNC,
  P_LFO_DIV,
  P_PB_RANGE,
  P_DLY_WET,
  P_DLY_FB,
  P_DLY_SYNC,
  P_DLY_DIV,
  P_DLY_TIME,
  P_DLY_DAMP,
  P_FLG_WET,
  P_FLG_RATE,
  P_FLG_DEPTH,
  P_FLG_FB,
  P_FLG_MANUAL,
  P_FLG_POLARITY,
  P_FLG_WIDTH,
  P_GLIDE,
  P_MASTER_VOL,
  // V0.14B LIGHT REVERB: append only, preserving all legacy ParamId values.
  P_REV_VOL,
  P_REV_SIZE,
  P_REV_DECAY,
  P_REV_DAMP,
  PARAM_COUNT
};

void miosaurusGuiCreate(lv_obj_t *parent);
void miosaurusGuiService();
void miosaurusGuiSetParam(MioParamId id, float normalized01);
float miosaurusGuiGetParam(MioParamId id);
void miosaurusGuiSetPreset(uint8_t slot, const char *name, bool edited);

// Integration hook for the next DSP migration stage.
void miosaurusGuiOnParamChanged(MioParamId id, float normalized01);
