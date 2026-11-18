/*
  RTAL MIOSAURUS performance layer
  z33 Performance + MIDI state layer.

  Presets are intentionally still excluded from this stage.
*/

#include "Z_MiosaurusPerformance.h"
#include "Z_MiosaurusAudio.h"
#include "Z_MiosaurusState.h"

#include "Board.h"
#include <SD.h>
#include <math.h>

namespace {

static bool sHeld[128] = {};
static uint8_t sHeldVel[128] = {};
static uint32_t sHeldOrder[128] = {};
static uint32_t sHeldCounter = 1;

static bool sPara = false;
static bool sLegato = false;
static MioNotePriority sPriority = MIO_PRIORITY_LAST;

static MioModRoute sVelRoute = MIO_ROUTE_FILTER_ENV;
static MioModRoute sAtRoute = MIO_ROUTE_CUTOFF;
static float sVelAmount = 0.65f;
static float sAtAmount = 0.50f;
static MioVelocityCurve sVelCurve = MIO_VEL_MID;

static float sPitchBendNorm = 0.0f;
static float sModWheelNorm = 0.0f;
static float sAftertouchNorm = 0.0f;

static uint8_t sCcBank = 0;

static float sMidiBpm = 100.0f;
static bool sClockSeen = false;
static uint32_t sClockWindowStartUs = 0;
static uint32_t sClockWindowTicks = 0;
static uint32_t sClockTotalTicks = 0;

static bool sParaRefreshPending = false;
static uint32_t sParaRefreshDeadlineUs = 0;

static constexpr uint8_t CC_NONE = 0xFF;
static uint8_t sLearnB0[MIO_LEARN_TARGET_COUNT];
static uint8_t sLearnB1[MIO_LEARN_TARGET_COUNT];

static volatile bool sLearnArmed = false;
static volatile uint8_t sLearnArmedBank = 0;
static volatile uint8_t sLearnArmedTarget = 0;
static volatile uint8_t sLearnLastCc = 0xFF;
static volatile uint32_t sLearnRevision = 1;

static portMUX_TYPE sPerfMux = portMUX_INITIALIZER_UNLOCKED;

static inline float clamp01(float x) {
  if (!isfinite(x)) return 0.0f;
  if (x < 0.0f) return 0.0f;
  if (x > 1.0f) return 1.0f;
  return x;
}

static bool isReservedCc(uint8_t cc) {
  return cc == 1 || cc == 32 || cc == 120 || cc == 123;
}

static bool isBank1FxParam(uint8_t target) {
  return (target >= P_DLY_WET && target <= P_DLY_DAMP) ||
         (target >= P_FLG_WET && target <= P_FLG_WIDTH) ||
         (target >= P_REV_VOL && target <= P_REV_DAMP);
}

static uint8_t *mapForBank(uint8_t bank) {
  return bank ? sLearnB1 : sLearnB0;
}

static constexpr const char *MIDI_LEARN_FILE = "/RTAL_MIOSAURUS/MIDI_LEARN.bin";
static constexpr uint32_t MIDI_LEARN_MAGIC = 0x4D494C4EUL; // "MILN"
static constexpr uint16_t MIDI_LEARN_VERSION = 2;
static constexpr uint16_t MIDI_LEARN_VERSION_V1 = 1;
static constexpr uint16_t MIO_LEARN_TARGET_COUNT_V1 = 52; // 44 params + 8 performance targets

struct __attribute__((packed)) MidiLearnFile {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint8_t bank0[MIO_LEARN_TARGET_COUNT];
  uint8_t bank1[MIO_LEARN_TARGET_COUNT];
  uint32_t crc32;
};

struct __attribute__((packed)) MidiLearnFileV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint8_t bank0[MIO_LEARN_TARGET_COUNT_V1];
  uint8_t bank1[MIO_LEARN_TARGET_COUNT_V1];
  uint32_t crc32;
};

static uint32_t learnCrc(const uint8_t *data, size_t len) {
  uint32_t crc=0xFFFFFFFFUL;
  for(size_t i=0;i<len;++i) {
    crc^=data[i];
    for(uint8_t b=0;b<8;++b)
      crc=(crc>>1)^(0xEDB88320UL & (uint32_t)-(int32_t)(crc&1U));
  }
  return ~crc;
}

static void saveLearnFile() {
  if(!Board::sdReady()) return;
  if(!SD.exists("/RTAL_MIOSAURUS")) SD.mkdir("/RTAL_MIOSAURUS");

  MidiLearnFile f{};
  f.magic=MIDI_LEARN_MAGIC;
  f.version=MIDI_LEARN_VERSION;
  f.count=MIO_LEARN_TARGET_COUNT;
  memcpy(f.bank0,sLearnB0,sizeof(sLearnB0));
  memcpy(f.bank1,sLearnB1,sizeof(sLearnB1));
  f.crc32=learnCrc((const uint8_t*)&f,offsetof(MidiLearnFile,crc32));

  const char *tmp="/RTAL_MIOSAURUS/MIDI_LEARN.tmp";
  SD.remove(tmp);
  File out=SD.open(tmp,FILE_WRITE);
  if(!out) return;
  bool ok=out.write((const uint8_t*)&f,sizeof(f))==sizeof(f);
  out.flush(); out.close();
  if(!ok) { SD.remove(tmp); return; }

  SD.remove(MIDI_LEARN_FILE);
  SD.rename(tmp,MIDI_LEARN_FILE);
}

static void loadLearnFile() {
  memset(sLearnB0,CC_NONE,sizeof(sLearnB0));
  memset(sLearnB1,CC_NONE,sizeof(sLearnB1));
  if(!Board::sdReady()) return;

  File in=SD.open(MIDI_LEARN_FILE,FILE_READ);
  if(!in) return;

  const size_t sz=(size_t)in.size();
  if(sz==sizeof(MidiLearnFile)) {
    MidiLearnFile f{};
    bool ok=in.read((uint8_t*)&f,sizeof(f))==(int)sizeof(f);
    in.close();
    if(!ok || f.magic!=MIDI_LEARN_MAGIC || f.version!=MIDI_LEARN_VERSION ||
       f.count!=MIO_LEARN_TARGET_COUNT) return;
    uint32_t crc=learnCrc((const uint8_t*)&f,offsetof(MidiLearnFile,crc32));
    if(crc!=f.crc32) return;
    memcpy(sLearnB0,f.bank0,sizeof(sLearnB0));
    memcpy(sLearnB1,f.bank1,sizeof(sLearnB1));
    return;
  }

  if(sz==sizeof(MidiLearnFileV1)) {
    MidiLearnFileV1 f{};
    bool ok=in.read((uint8_t*)&f,sizeof(f))==(int)sizeof(f);
    in.close();
    if(!ok || f.magic!=MIDI_LEARN_MAGIC || f.version!=MIDI_LEARN_VERSION_V1 ||
       f.count!=MIO_LEARN_TARGET_COUNT_V1) return;
    uint32_t crc=learnCrc((const uint8_t*)&f,offsetof(MidiLearnFileV1,crc32));
    if(crc!=f.crc32) return;

    // Param IDs 0..43 are unchanged. Performance learn targets moved by +4
    // because the four reverb params are appended before them.
    for(uint8_t i=0;i<44;i++) { sLearnB0[i]=f.bank0[i]; sLearnB1[i]=f.bank1[i]; }
    for(uint8_t i=0;i<8;i++) {
      sLearnB0[PARAM_COUNT+i]=f.bank0[44+i];
      sLearnB1[PARAM_COUNT+i]=f.bank1[44+i];
    }
    // Persist migrated mapping in V2 format on the next explicit learn change.
    return;
  }

  in.close();
}

static uint8_t defaultCc(uint8_t bank, uint8_t target) {
  if (bank == 0) {
    if (target < PARAM_COUNT && !isBank1FxParam(target)) {
      return miosaurusStateDef((MioParamId)target).defaultCc;
    }
    return CC_NONE;
  }

  switch (target) {
    case P_DLY_WET: return 106;
    case P_DLY_FB: return 109;
    case P_DLY_SYNC: return 110;
    case P_DLY_DIV: return 111;
    case P_DLY_TIME: return 112;
    case P_DLY_DAMP: return 115;

    case P_FLG_WET: return 28;
    case P_FLG_RATE: return 52;
    case P_FLG_DEPTH: return 29;
    case P_FLG_FB: return 22;
    case P_FLG_MANUAL: return 23;
    case P_FLG_POLARITY: return 20;
    case P_FLG_WIDTH: return 30;

    case P_REV_VOL: return 118;
    case P_REV_SIZE: return 119;
    case P_REV_DECAY: return 75;
    case P_REV_DAMP: return 76;

    case MIO_LT_LEGATO: return 102;
    case MIO_LT_PRIORITY: return 103;
    case MIO_LT_VEL_CURVE: return 86;
    case MIO_LT_PARA: return 105;
    case MIO_LT_VEL_ROUTE: return 89;
    case MIO_LT_AT_ROUTE: return 90;
    case MIO_LT_VEL_AMT: return 77;
    case MIO_LT_AT_AMT: return 78;
    default: return CC_NONE;
  }
}

static int8_t lowestHeld() {
  for (int n = 0; n < 128; ++n) if (sHeld[n]) return (int8_t)n;
  return -1;
}

static int8_t highestHeld() {
  for (int n = 127; n >= 0; --n) if (sHeld[n]) return (int8_t)n;
  return -1;
}

static int8_t lastHeld() {
  int8_t best = -1;
  uint32_t bestOrder = 0;
  for (int n = 0; n < 128; ++n) {
    if (sHeld[n] && sHeldOrder[n] >= bestOrder) {
      bestOrder = sHeldOrder[n];
      best = (int8_t)n;
    }
  }
  return best;
}

static int8_t secondLowestHeld() {
  bool first = false;
  for (int n = 0; n < 128; ++n) {
    if (!sHeld[n]) continue;
    if (!first) { first = true; continue; }
    return (int8_t)n;
  }
  return -1;
}

static int8_t secondHighestHeld() {
  bool first = false;
  for (int n = 127; n >= 0; --n) {
    if (!sHeld[n]) continue;
    if (!first) { first = true; continue; }
    return (int8_t)n;
  }
  return -1;
}

static void lastTwoHeld(int8_t &n1, int8_t &n2) {
  n1 = -1; n2 = -1;
  uint32_t best1 = 0, best2 = 0;
  for (int n = 0; n < 128; ++n) {
    if (!sHeld[n]) continue;
    uint32_t ord = sHeldOrder[n];
    if (ord >= best1) {
      best2 = best1; n2 = n1;
      best1 = ord; n1 = (int8_t)n;
    } else if (ord >= best2) {
      best2 = ord; n2 = (int8_t)n;
    }
  }
}

static int countHeld() {
  int c = 0;
  for (int n = 0; n < 128; ++n) if (sHeld[n]) ++c;
  return c;
}

static int8_t selectedHeld() {
  switch (sPriority) {
    case MIO_PRIORITY_LOW: return lowestHeld();
    case MIO_PRIORITY_HIGH: return highestHeld();
    case MIO_PRIORITY_LAST:
    default: return lastHeld();
  }
}

static void selectTwo(int8_t &n1, int8_t &n2) {
  switch (sPriority) {
    case MIO_PRIORITY_LOW:
      n1 = lowestHeld(); n2 = secondLowestHeld(); break;
    case MIO_PRIORITY_HIGH:
      n1 = highestHeld(); n2 = secondHighestHeld(); break;
    case MIO_PRIORITY_LAST:
    default:
      lastTwoHeld(n1, n2); break;
  }
}

static void clearHeld() {
  memset(sHeld, 0, sizeof(sHeld));
  memset(sHeldVel, 0, sizeof(sHeldVel));
  memset(sHeldOrder, 0, sizeof(sHeldOrder));
}

static bool anyHeld() {
  for (int n = 0; n < 128; ++n) if (sHeld[n]) return true;
  return false;
}

static void refreshVoices(bool triggeredByNoteOn) {
  if (!sPara) {
    int8_t note = selectedHeld();
    if (note < 0) {
      miosaurusAudioReleaseAll();
      return;
    }
    const bool retrigger = triggeredByNoteOn && !sLegato;
    // When there was no previous gate, Audio always starts its envelopes.
    miosaurusAudioSetMonoNote((uint8_t)note, sHeldVel[(uint8_t)note], retrigger);
    return;
  }

  if (!anyHeld()) {
    miosaurusAudioReleaseAll();
    return;
  }

  const int heldCount = countHeld();
  int8_t n1 = -1, n2 = -1;
  selectTwo(n1, n2);
  int8_t sub = lowestHeld();

  uint8_t v1 = (n1 >= 0) ? sHeldVel[(uint8_t)n1] : 100;
  uint8_t v2 = (n2 >= 0) ? sHeldVel[(uint8_t)n2] : v1;

  bool secondActive = heldCount >= 2 && n2 >= 0 && n2 != n1;
  bool singleUnison = heldCount == 1 && triggeredByNoteOn;
  bool retrigger = triggeredByNoteOn && !sLegato;

  miosaurusAudioSetParaNotes(n1, secondActive ? n2 : -1, sub,
                             v1, v2, retrigger, singleUnison);
}

static MioModRoute decodeVelRoute(uint8_t v) {
  static const MioModRoute opt[] = {
    MIO_ROUTE_OFF, MIO_ROUTE_FILTER_ENV, MIO_ROUTE_CUTOFF,
    MIO_ROUTE_DRIVE, MIO_ROUTE_PW1, MIO_ROUTE_PW2
  };
  uint8_t idx = (uint8_t)((v * (uint16_t)(sizeof(opt)/sizeof(opt[0]))) / 128U);
  if (idx >= sizeof(opt)/sizeof(opt[0])) idx = sizeof(opt)/sizeof(opt[0]) - 1;
  return opt[idx];
}

static MioModRoute decodeAtRoute(uint8_t v) {
  static const MioModRoute opt[] = {
    MIO_ROUTE_OFF, MIO_ROUTE_CUTOFF, MIO_ROUTE_RESO,
    MIO_ROUTE_LFO_FILTER, MIO_ROUTE_DRIVE, MIO_ROUTE_PW1, MIO_ROUTE_PW2
  };
  uint8_t idx = (uint8_t)((v * (uint16_t)(sizeof(opt)/sizeof(opt[0]))) / 128U);
  if (idx >= sizeof(opt)/sizeof(opt[0])) idx = sizeof(opt)/sizeof(opt[0]) - 1;
  return opt[idx];
}

static bool applyTarget(uint8_t target, uint8_t value) {
  if (target < PARAM_COUNT) {
    return miosaurusStateQueueNormalized((MioParamId)target, value / 127.0f);
  }

  switch (target) {
    case MIO_LT_LEGATO:
      miosaurusPerformanceSetLegato(value >= 64); return true;
    case MIO_LT_PRIORITY:
      miosaurusPerformanceSetPriority(
          value <= 42 ? MIO_PRIORITY_LAST :
          value <= 84 ? MIO_PRIORITY_LOW : MIO_PRIORITY_HIGH);
      return true;
    case MIO_LT_PARA:
      miosaurusPerformanceSetPara(value >= 64); return true;
    case MIO_LT_VEL_ROUTE:
      miosaurusPerformanceSetVelocityRoute(decodeVelRoute(value)); return true;
    case MIO_LT_AT_ROUTE:
      miosaurusPerformanceSetAftertouchRoute(decodeAtRoute(value)); return true;
    case MIO_LT_VEL_AMT:
      miosaurusPerformanceSetVelocityAmount(value / 127.0f); return true;
    case MIO_LT_AT_AMT:
      miosaurusPerformanceSetAftertouchAmount(value / 127.0f); return true;
    case MIO_LT_VEL_CURVE:
      miosaurusPerformanceSetVelocityCurve(
          value <= 42 ? MIO_VEL_SOFT : value <= 84 ? MIO_VEL_MID : MIO_VEL_HARD);
      return true;
    default:
      return false;
  }
}

static int resolveTarget(uint8_t bank, uint8_t cc) {
  if (isReservedCc(cc)) return -1;
  uint8_t *map = mapForBank(bank);

  for (uint8_t t = 0; t < MIO_LEARN_TARGET_COUNT; ++t) {
    if (!miosaurusMidiLearnAllowed(bank, t)) continue;
    if (map[t] != CC_NONE && map[t] == cc) return t;
  }
  for (uint8_t t = 0; t < MIO_LEARN_TARGET_COUNT; ++t) {
    if (!miosaurusMidiLearnAllowed(bank, t)) continue;
    if (map[t] != CC_NONE) continue;
    if (defaultCc(bank, t) == cc) return t;
  }
  return -1;
}

} // namespace

bool miosaurusPerformanceBegin() {
  clearHeld();
  loadLearnFile();
  return true;
}

void miosaurusPerformanceService() {
  if (sParaRefreshPending &&
      (int32_t)(micros() - sParaRefreshDeadlineUs) >= 0) {
    sParaRefreshPending = false;
    refreshVoices(false);
  }
}

void miosaurusPerformanceNoteOn(uint8_t note, uint8_t velocity) {
  if (velocity == 0) {
    miosaurusPerformanceNoteOff(note);
    return;
  }
  sParaRefreshPending = false;
  sHeld[note] = true;
  sHeldVel[note] = velocity;
  sHeldOrder[note] = sHeldCounter++;
  refreshVoices(true);
}

void miosaurusPerformanceNoteOff(uint8_t note) {
  sHeld[note] = false;
  sHeldVel[note] = 0;
  sHeldOrder[note] = 0;

  if (sPara) {
    sParaRefreshPending = true;
    sParaRefreshDeadlineUs = micros() + 4000;
  } else {
    refreshVoices(false);
  }
}

void miosaurusPerformancePanic() {
  sParaRefreshPending = false;
  clearHeld();
  sAftertouchNorm = 0.0f;
  miosaurusAudioReleaseAll();
}

void miosaurusPerformancePitchBend(int16_t bend) {
  float n = (bend >= 0) ? ((float)bend / 8191.0f)
                         : ((float)bend / 8192.0f);
  portENTER_CRITICAL(&sPerfMux);
  sPitchBendNorm = constrain(n, -1.0f, 1.0f);
  portEXIT_CRITICAL(&sPerfMux);
}

void miosaurusPerformanceModWheel(uint8_t value) {
  portENTER_CRITICAL(&sPerfMux);
  sModWheelNorm = value / 127.0f;
  portEXIT_CRITICAL(&sPerfMux);
}

void miosaurusPerformanceAftertouch(uint8_t pressure) {
  portENTER_CRITICAL(&sPerfMux);
  sAftertouchNorm = pressure / 127.0f;
  portEXIT_CRITICAL(&sPerfMux);
}

void miosaurusPerformanceClock() {
  uint32_t now = micros();
  if (sClockWindowStartUs == 0) {
    sClockWindowStartUs = now;
    sClockWindowTicks = 0;
  }
  ++sClockWindowTicks;
  ++sClockTotalTicks;
  if (sClockWindowTicks >= 24) {
    uint32_t dt = now - sClockWindowStartUs;
    if (dt > 0) {
      float measured = 60.0f * 1000000.0f / (float)dt;
      sMidiBpm += 0.15f * (measured - sMidiBpm);
      sClockSeen = true;
    }
    sClockWindowStartUs = now;
    sClockWindowTicks = 0;
  }

  if (miosaurusStateGetActual(P_LFO_SYNC) >= 0.5f) {
    int idx = (int)(miosaurusStateGetNormalized(P_LFO_DIV) * 10.999f);
    idx = constrain(idx, 0, 10);
    static const uint32_t ticks[11] = {4,6,9,8,12,18,16,24,36,48,96};
    uint32_t cyc = ticks[idx];
    if (cyc && (sClockTotalTicks % cyc) == 0) miosaurusAudioResetLfo();
  }
}

void miosaurusPerformanceStart() {
  sParaRefreshPending = false;
  sAftertouchNorm = 0.0f;
  sClockWindowStartUs = 0;
  sClockWindowTicks = 0;
  sClockTotalTicks = 0;
  sClockSeen = false;
  miosaurusAudioResetFx();
  miosaurusAudioResetLfo();
}

void miosaurusPerformanceStop() {
  sParaRefreshPending = false;
  sAftertouchNorm = 0.0f;
  clearHeld();
  miosaurusAudioResetFx();
  miosaurusAudioReleaseAll();
}

void miosaurusPerformanceTogglePara() {
  miosaurusPerformanceSetPara(!sPara);
}
void miosaurusPerformanceToggleLegato() {
  miosaurusPerformanceSetLegato(!sLegato);
}
void miosaurusPerformanceCyclePriority() {
  miosaurusPerformanceSetPriority(
      sPriority == MIO_PRIORITY_LAST ? MIO_PRIORITY_LOW :
      sPriority == MIO_PRIORITY_LOW ? MIO_PRIORITY_HIGH : MIO_PRIORITY_LAST);
}
void miosaurusPerformanceToggleCcBank() {
  miosaurusPerformanceSetCcBank(sCcBank ? 0 : 1);
}

bool miosaurusPerformancePara() { return sPara; }
bool miosaurusPerformanceLegato() { return sLegato; }
MioNotePriority miosaurusPerformancePriority() { return sPriority; }
const char *miosaurusPerformancePriorityName() {
  return sPriority == MIO_PRIORITY_LOW ? "LOW" :
         sPriority == MIO_PRIORITY_HIGH ? "HIGH" : "LAST";
}
uint8_t miosaurusPerformanceCcBank() { return sCcBank; }

float miosaurusPerformancePitchBendNorm() { return sPitchBendNorm; }
float miosaurusPerformancePitchBendRatio(float rangeSemi) {
  return powf(2.0f, sPitchBendNorm * constrain(rangeSemi, 0.0f, 12.0f) / 12.0f);
}
float miosaurusPerformanceModWheelNorm() { return sModWheelNorm; }
float miosaurusPerformanceAftertouchNorm() { return sAftertouchNorm; }
float miosaurusPerformanceBpm() { return sMidiBpm; }
bool miosaurusPerformanceClockSeen() { return sClockSeen; }

MioModRoute miosaurusPerformanceVelocityRoute() { return sVelRoute; }
MioModRoute miosaurusPerformanceAftertouchRoute() { return sAtRoute; }
float miosaurusPerformanceVelocityAmount() { return sVelAmount; }
float miosaurusPerformanceAftertouchAmount() { return sAtAmount; }
MioVelocityCurve miosaurusPerformanceVelocityCurve() { return sVelCurve; }

void miosaurusPerformanceSetPara(bool enabled) {
  sParaRefreshPending = false;
  sPara = enabled;
  refreshVoices(false);
}
void miosaurusPerformanceSetLegato(bool enabled) { sLegato = enabled; }
void miosaurusPerformanceSetPriority(MioNotePriority mode) {
  sParaRefreshPending = false;
  sPriority = mode;
  refreshVoices(false);
}
void miosaurusPerformanceSetVelocityRoute(MioModRoute route) { sVelRoute = route; }
void miosaurusPerformanceSetAftertouchRoute(MioModRoute route) { sAtRoute = route; }
void miosaurusPerformanceSetVelocityAmount(float amount) { sVelAmount = clamp01(amount); }
void miosaurusPerformanceSetAftertouchAmount(float amount) { sAtAmount = clamp01(amount); }
void miosaurusPerformanceSetVelocityCurve(MioVelocityCurve curve) {
  sVelCurve = (curve > MIO_VEL_HARD) ? MIO_VEL_MID : curve;
}
void miosaurusPerformanceSetCcBank(uint8_t bank) { sCcBank = bank ? 1 : 0; }

void miosaurusMidiLearnArm(uint8_t bank, uint8_t target) {
  bank = bank ? 1 : 0;
  if (target >= MIO_LEARN_TARGET_COUNT || !miosaurusMidiLearnAllowed(bank, target)) return;
  sLearnArmedBank = bank;
  sLearnArmedTarget = target;
  sLearnLastCc = 0xFF;
  sLearnArmed = true;
  ++sLearnRevision;
}

void miosaurusMidiLearnCancel() {
  if (sLearnArmed) {
    sLearnArmed = false;
    ++sLearnRevision;
  }
}

bool miosaurusMidiLearnArmed() { return sLearnArmed; }
uint8_t miosaurusMidiLearnLastCc() { return sLearnLastCc; }
uint32_t miosaurusMidiLearnRevision() { return sLearnRevision; }

bool miosaurusMidiLearnAllowed(uint8_t bank, uint8_t target) {
  if (bank == 0) return target < PARAM_COUNT && !isBank1FxParam(target);
  if (isBank1FxParam(target)) return true;
  return target == MIO_LT_LEGATO || target == MIO_LT_PRIORITY ||
         target == MIO_LT_PARA || target == MIO_LT_VEL_ROUTE ||
         target == MIO_LT_AT_ROUTE || target == MIO_LT_VEL_AMT ||
         target == MIO_LT_AT_AMT || target == MIO_LT_VEL_CURVE;
}

uint8_t miosaurusMidiEffectiveCc(uint8_t bank, uint8_t target) {
  if (target >= MIO_LEARN_TARGET_COUNT) return CC_NONE;
  uint8_t learned = mapForBank(bank)[target];
  return learned != CC_NONE ? learned : defaultCc(bank, target);
}

bool miosaurusMidiAssignLearn(uint8_t bank, uint8_t target, uint8_t cc) {
  bank = bank ? 1 : 0;
  if (target >= MIO_LEARN_TARGET_COUNT || !miosaurusMidiLearnAllowed(bank, target) ||
      isReservedCc(cc)) return false;
  uint8_t *map = mapForBank(bank);
  for (uint8_t t = 0; t < MIO_LEARN_TARGET_COUNT; ++t) {
    if (t != target && map[t] == cc) {
      map[t] = CC_NONE;
    }
  }
  map[target] = cc;
  saveLearnFile();
  ++sLearnRevision;
  return true;
}

void miosaurusMidiClearLearn(uint8_t bank, uint8_t target) {
  bank = bank ? 1 : 0;
  if (target >= MIO_LEARN_TARGET_COUNT) return;
  mapForBank(bank)[target] = CC_NONE;
  saveLearnFile();
  ++sLearnRevision;
}

void miosaurusMidiResetLearnBank(uint8_t bank) {
  bank = bank ? 1 : 0;
  uint8_t *map = mapForBank(bank);
  for (uint8_t t = 0; t < MIO_LEARN_TARGET_COUNT; ++t) map[t] = CC_NONE;
  saveLearnFile();
  ++sLearnRevision;
}

bool miosaurusMidiApplyCC(uint8_t cc, uint8_t value) {
  if (sLearnArmed && !isReservedCc(cc)) {
    const uint8_t bank = sLearnArmedBank;
    const uint8_t target = sLearnArmedTarget;
    if (miosaurusMidiAssignLearn(bank, target, cc)) {
      sLearnLastCc = cc;
      sLearnArmed = false;
      ++sLearnRevision;
      applyTarget(target, value);
      return true;
    }
  }

  if (cc == 32) {
    // V0.14B FIX6: CC32 is an exact bank selector, not a binary threshold.
    // MIOSAURUS currently implements two CC banks: 0 = B0, 1 = B1.
    // Other values (for example 2 from controllers with a third bank) are
    // intentionally ignored until a real B2 mapping is defined.
    if (value == 0 || value == 1) {
      miosaurusPerformanceSetCcBank(value);
      return true;
    }
    return false;
  }
  if (cc == 1) {
    miosaurusPerformanceModWheel(value);
    return true;
  }
  if (cc == 120 || cc == 123) {
    miosaurusPerformancePanic();
    return true;
  }

  int target = resolveTarget(sCcBank, cc);
  if (target >= 0) return applyTarget((uint8_t)target, value);
  return false;
}


void miosaurusPerformanceSnapshotPreset(MioPerformancePresetState &out) {
  out.legato = sLegato;
  out.priority = (uint8_t)sPriority;
  out.para = sPara;
  out.velocityRoute = (uint8_t)sVelRoute;
  out.aftertouchRoute = (uint8_t)sAtRoute;
  out.velocityAmount = sVelAmount;
  out.aftertouchAmount = sAtAmount;
  out.velocityCurve = (uint8_t)sVelCurve;
}

void miosaurusPerformanceResetForPresetLoad() {
  sParaRefreshPending = false;
  clearHeld();
  sAftertouchNorm = 0.0f;
  // V0.13C FIX2: do NOT hard-reset FX buffers on a preset load.
  // PingPongDelay::reset() clears two 2-second float buffers (~512 kB PSRAM)
  // inside the high-priority audio task and caused one 20-30 ms render spike
  // per applied preset. New preset FX parameters are picked up at the next
  // audio block; allowing the old tail to decay is click-free and realtime-safe.
  miosaurusAudioReleaseAll();
}

void miosaurusPerformanceApplyPreset(const MioPerformancePresetState &in) {
  sParaRefreshPending = false;

  sLegato = in.legato;
  sPriority = (in.priority <= (uint8_t)MIO_PRIORITY_HIGH)
            ? (MioNotePriority)in.priority
            : MIO_PRIORITY_LAST;
  sPara = in.para;

  uint8_t vr = in.velocityRoute;
  if (vr > (uint8_t)MIO_ROUTE_LFO_FILTER) vr = (uint8_t)MIO_ROUTE_FILTER_ENV;
  uint8_t ar = in.aftertouchRoute;
  if (ar > (uint8_t)MIO_ROUTE_LFO_FILTER) ar = (uint8_t)MIO_ROUTE_CUTOFF;

  sVelRoute = (MioModRoute)vr;
  sAtRoute = (MioModRoute)ar;
  sVelAmount = clamp01(in.velocityAmount);
  sAtAmount = clamp01(in.aftertouchAmount);
  sVelCurve = (in.velocityCurve <= (uint8_t)MIO_VEL_HARD)
            ? (MioVelocityCurve)in.velocityCurve
            : MIO_VEL_MID;

  sAftertouchNorm = 0.0f;
}
