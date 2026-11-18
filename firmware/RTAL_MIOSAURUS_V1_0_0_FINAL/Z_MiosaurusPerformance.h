#pragma once
#include <Arduino.h>
#include "Z_MiosaurusGui.h"

enum MioNotePriority : uint8_t {
  MIO_PRIORITY_LAST = 0,
  MIO_PRIORITY_LOW  = 1,
  MIO_PRIORITY_HIGH = 2
};

enum MioModRoute : uint8_t {
  MIO_ROUTE_OFF = 0,
  MIO_ROUTE_CUTOFF = 1,
  MIO_ROUTE_RESO = 2,
  MIO_ROUTE_DRIVE = 3,
  MIO_ROUTE_FILTER_ENV = 4,
  MIO_ROUTE_PW1 = 5,
  MIO_ROUTE_PW2 = 6,
  MIO_ROUTE_LFO_FILTER = 7
};

enum MioVelocityCurve : uint8_t {
  MIO_VEL_SOFT = 0,
  MIO_VEL_MID  = 1,
  MIO_VEL_HARD = 2
};

// Additional z33 MIDI-learn target IDs after PARAM_COUNT.
enum MioLearnTarget : uint8_t {
  MIO_LT_LEGATO = PARAM_COUNT,
  MIO_LT_PRIORITY,
  MIO_LT_PARA,
  MIO_LT_VEL_ROUTE,
  MIO_LT_AT_ROUTE,
  MIO_LT_VEL_AMT,
  MIO_LT_AT_AMT,
  MIO_LT_VEL_CURVE,
  MIO_LEARN_TARGET_COUNT
};

bool miosaurusPerformanceBegin();
void miosaurusPerformanceService();

// MIDI performance events
void miosaurusPerformanceNoteOn(uint8_t note, uint8_t velocity);
void miosaurusPerformanceNoteOff(uint8_t note);
void miosaurusPerformancePanic();
void miosaurusPerformancePitchBend(int16_t bend);
void miosaurusPerformanceModWheel(uint8_t value);
void miosaurusPerformanceAftertouch(uint8_t pressure);
void miosaurusPerformanceClock();
void miosaurusPerformanceStart();
void miosaurusPerformanceStop();

// Touch/UI performance controls
void miosaurusPerformanceTogglePara();
void miosaurusPerformanceToggleLegato();
void miosaurusPerformanceCyclePriority();
void miosaurusPerformanceToggleCcBank();

bool miosaurusPerformancePara();
bool miosaurusPerformanceLegato();
MioNotePriority miosaurusPerformancePriority();
const char *miosaurusPerformancePriorityName();
uint8_t miosaurusPerformanceCcBank();

float miosaurusPerformancePitchBendNorm();
float miosaurusPerformancePitchBendRatio(float rangeSemi);
float miosaurusPerformanceModWheelNorm();
float miosaurusPerformanceAftertouchNorm();
float miosaurusPerformanceBpm();
bool miosaurusPerformanceClockSeen();

MioModRoute miosaurusPerformanceVelocityRoute();
MioModRoute miosaurusPerformanceAftertouchRoute();
float miosaurusPerformanceVelocityAmount();
float miosaurusPerformanceAftertouchAmount();
MioVelocityCurve miosaurusPerformanceVelocityCurve();

void miosaurusPerformanceSetPara(bool enabled);
void miosaurusPerformanceSetLegato(bool enabled);
void miosaurusPerformanceSetPriority(MioNotePriority mode);
void miosaurusPerformanceSetVelocityRoute(MioModRoute route);
void miosaurusPerformanceSetAftertouchRoute(MioModRoute route);
void miosaurusPerformanceSetVelocityAmount(float amount);
void miosaurusPerformanceSetAftertouchAmount(float amount);
void miosaurusPerformanceSetVelocityCurve(MioVelocityCurve curve);
void miosaurusPerformanceSetCcBank(uint8_t bank);

// CC mapping / learn infrastructure
bool miosaurusMidiApplyCC(uint8_t cc, uint8_t value);
bool miosaurusMidiAssignLearn(uint8_t bank, uint8_t target, uint8_t cc);
void miosaurusMidiClearLearn(uint8_t bank, uint8_t target);
void miosaurusMidiResetLearnBank(uint8_t bank);
uint8_t miosaurusMidiEffectiveCc(uint8_t bank, uint8_t target);
bool miosaurusMidiLearnAllowed(uint8_t bank, uint8_t target);

void miosaurusMidiLearnArm(uint8_t bank, uint8_t target);
void miosaurusMidiLearnCancel();
bool miosaurusMidiLearnArmed();
uint8_t miosaurusMidiLearnLastCc();
uint32_t miosaurusMidiLearnRevision();


struct MioPerformancePresetState {
  bool legato;
  uint8_t priority;
  bool para;
  uint8_t velocityRoute;
  uint8_t aftertouchRoute;
  float velocityAmount;
  float aftertouchAmount;
  uint8_t velocityCurve;
};

void miosaurusPerformanceSnapshotPreset(MioPerformancePresetState &out);
void miosaurusPerformanceApplyPreset(const MioPerformancePresetState &in);
void miosaurusPerformanceResetForPresetLoad();
