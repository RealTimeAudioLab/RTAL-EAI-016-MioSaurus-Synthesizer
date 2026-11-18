/*
  RTAL MIOSAURUS preset layer
  128 named SD-card presets, using robust SD-card storage.

  Paths:
    /RTAL_MIOSAURUS/PRESETS/P000.rtal ... P127.rtal

  No Preferences/NVS is used by this preset layer.

  File safety:
    - fixed header
    - version + header/payload size checks
    - CRC32 over complete payload
    - write .tmp
    - verify .tmp
    - rotate old final to .bak
    - rename .tmp to final
    - restore .bak if final rename fails
*/

#include "Z_MiosaurusPreset.h"
#include "Z_MiosaurusGui.h"
#include "Z_MiosaurusState.h"
#include "Z_MiosaurusPerformance.h"
#include "Board.h"

#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>

namespace {

static constexpr const char *MIO_ROOT = "/RTAL_MIOSAURUS";
static constexpr const char *MIO_PRESET_DIR = "/RTAL_MIOSAURUS/PRESETS";
static constexpr const char *MIO_LAST_FILE = "/RTAL_MIOSAURUS/LAST_SLOT.bin";

static constexpr uint32_t PRESET_MAGIC = 0x4D494F50UL; // "MIOP"
static constexpr uint16_t PRESET_VERSION = 2;
static constexpr uint16_t PRESET_VERSION_V1 = 1;
static constexpr uint32_t LAST_MAGIC = 0x4D494F4CUL;   // "MIOL"
static constexpr uint16_t LAST_VERSION = 1;

struct __attribute__((packed)) PresetHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t headerSize;
  uint32_t payloadSize;
  uint32_t crc32;
};

// V1 payload retained verbatim for backwards-compatible loading.
// V0.14B appends four reverb parameters; all earlier ParamIds remain unchanged.
static constexpr size_t PARAM_COUNT_V1 = 44;
struct __attribute__((packed)) PresetPayloadV1 {
  char name[MIO_PRESET_NAME_MAX + 1];
  float values[PARAM_COUNT_V1];
  uint8_t legato;
  uint8_t priority;
  uint8_t para;
  uint8_t velocityRoute;
  uint8_t aftertouchRoute;
  uint8_t velocityCurve;
  uint8_t reserved0;
  uint8_t reserved1;
  float velocityAmount;
  float aftertouchAmount;
};

struct __attribute__((packed)) PresetPayload {
  char name[MIO_PRESET_NAME_MAX + 1];
  float values[PARAM_COUNT];

  uint8_t legato;
  uint8_t priority;
  uint8_t para;
  uint8_t velocityRoute;
  uint8_t aftertouchRoute;
  uint8_t velocityCurve;
  uint8_t reserved0;
  uint8_t reserved1;

  float velocityAmount;
  float aftertouchAmount;
};

struct __attribute__((packed)) LastSlotFile {
  uint32_t magic;
  uint16_t version;
  uint8_t slot;
  uint8_t reserved;
  uint32_t crc32;
};

static SemaphoreHandle_t sMutex = nullptr;

static uint8_t sLoadedSlot = 0;
static uint8_t sBrowseSlot = 0;
static bool sEdited = false;
static bool sCurrentExists = false;
static char sLoadedName[MIO_PRESET_NAME_MAX + 1] = "INIT PATCH";

static float sBaseline[PARAM_COUNT] = {};
static MioPerformancePresetState sBaselinePerf = {};

static char sStatus[16] = "SD";
static uint32_t sStatusUntilMs = 0;
static volatile uint32_t sRevision = 1;
static uint32_t sLastEditedCheckMs = 0;

// FIX8: Saving must not block the playing/control path.
// The current preset is snapshotted immediately; SD/FAT operations run in a
// low-priority worker on Core0.
struct SaveJob {
  bool pending;
  bool saveAs;
  uint8_t slot;
  PresetPayload payload;
};

static SaveJob sSaveJob = {};
static SemaphoreHandle_t sSaveJobMutex = nullptr;
static TaskHandle_t sSaveTaskHandle = nullptr;
static bool sLastSlotWriteJobPending = false;
static uint8_t sLastSlotWriteJobSlot = 0;

// V0.13C FIX1: Runtime preset LOADS use a low-priority Core0 worker too.
// SD/FAT access is separated from the short preset-state apply step that is
// consumed later by MioControl. A request ID prevents an obsolete slow read
// from being applied after a newer preset request has arrived.
struct LoadJob {
  bool pending;
  bool showStatus;
  bool programChange;
  uint8_t slot;
  uint32_t requestId;
};

struct LoadResult {
  bool ready;
  bool ok;
  bool showStatus;
  bool programChange;
  uint8_t slot;
  uint32_t requestId;
  uint32_t loadUs;
  PresetPayload payload;
};

static LoadJob sLoadJob = {};
static LoadResult sLoadResult = {};
static SemaphoreHandle_t sLoadJobMutex = nullptr;
static TaskHandle_t sLoadTaskHandle = nullptr;
static volatile uint32_t sLatestLoadRequestId = 0;

// V0.13C FIX1: The GUI must not open preset files while audio is running.
// Names/existence are cached once during startup (before the audio task starts)
// and refreshed after successful saves/loads.
static bool sSlotCacheValid[MIO_PRESET_SLOT_COUNT] = {};
static bool sSlotCacheExists[MIO_PRESET_SLOT_COUNT] = {};
static char sSlotCacheName[MIO_PRESET_SLOT_COUNT][MIO_PRESET_NAME_MAX + 1] = {};

// FIX4: MIDI Program Change must never perform SD I/O in the MIDI parser.
// Fast bursts are coalesced to the most recently requested program.
static volatile bool sProgramPending = false;
static volatile uint8_t sPendingProgram = 0;
static volatile uint32_t sProgramRequestMs = 0;
static constexpr uint32_t PROGRAM_CHANGE_QUIET_MS = 45;

// LAST_SLOT persistence is deliberately delayed. During rapid program browsing
// this prevents an SD remove/open/write/flush cycle for every single PC.
static bool sLastSlotDirty = false;
static uint32_t sLastSlotDirtySinceMs = 0;
static constexpr uint32_t LAST_SLOT_PERSIST_DELAY_MS = 1500;

static volatile uint32_t sDiagPcRequests = 0;
static volatile uint32_t sDiagPcLoads = 0;
static volatile uint32_t sDiagLoadOk = 0;
static volatile uint32_t sDiagLoadFail = 0;
static volatile uint32_t sDiagLoadUsMax = 0;
static volatile uint32_t sDiagSaveOk = 0;
static volatile uint32_t sDiagSaveFail = 0;
static volatile uint32_t sDiagSaveUsMax = 0;

static inline bool validSlot(uint16_t slot) {
  return slot < MIO_PRESET_SLOT_COUNT;
}

static void bumpRevision() {
  ++sRevision;
  if (sRevision == 0) sRevision = 1;
}

static void setStatus(const char *txt, uint32_t ms = 1400) {
  strncpy(sStatus, txt ? txt : "", sizeof(sStatus) - 1);
  sStatus[sizeof(sStatus) - 1] = '\0';
  sStatusUntilMs = millis() + ms;
  bumpRevision();
}

static void sanitizeName(const char *src, char *dst, size_t dstLen) {
  if (!dst || dstLen == 0) return;
  dst[0] = '\0';
  if (!src) src = "";

  size_t w = 0;
  for (size_t i = 0; src[i] && w + 1 < dstLen; ++i) {
    unsigned char c = (unsigned char)src[i];
    // Keep ASCII printable names deterministic on the embedded UI/file format.
    if (c >= 32 && c <= 126) {
      dst[w++] = (char)c;
    }
  }
  while (w > 0 && dst[w - 1] == ' ') --w;
  size_t start = 0;
  while (start < w && dst[start] == ' ') ++start;
  if (start > 0) {
    memmove(dst, dst + start, w - start);
    w -= start;
  }
  dst[w] = '\0';

  if (w == 0) {
    strncpy(dst, "INIT PATCH", dstLen - 1);
    dst[dstLen - 1] = '\0';
  }
}

static void slotPath(uint8_t slot, char *out, size_t outLen) {
  snprintf(out, outLen, "%s/P%03u.rtal", MIO_PRESET_DIR, (unsigned)slot);
}
static void tmpPath(uint8_t slot, char *out, size_t outLen) {
  snprintf(out, outLen, "%s/P%03u.tmp", MIO_PRESET_DIR, (unsigned)slot);
}
static void bakPath(uint8_t slot, char *out, size_t outLen) {
  snprintf(out, outLen, "%s/P%03u.bak", MIO_PRESET_DIR, (unsigned)slot);
}

static uint32_t crc32Bytes(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1U));
    }
  }
  return ~crc;
}

static bool ensureDirectories() {
  if (!Board::sdReady()) return false;

  if (!SD.exists(MIO_ROOT)) {
    if (!SD.mkdir(MIO_ROOT)) return false;
  }
  if (!SD.exists(MIO_PRESET_DIR)) {
    if (!SD.mkdir(MIO_PRESET_DIR)) return false;
  }
  return true;
}

static bool readPresetFile(uint8_t slot, PresetHeader &h, PresetPayload &p) {
  if (!validSlot(slot) || !Board::sdReady()) return false;

  char path[64];
  slotPath(slot, path, sizeof(path));
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  bool ok = f.read((uint8_t *)&h, sizeof(h)) == (int)sizeof(h);
  if (!ok || h.magic != PRESET_MAGIC || h.headerSize != sizeof(PresetHeader)) {
    f.close();
    return false;
  }

  memset(&p, 0, sizeof(p));
  if (h.version == PRESET_VERSION && h.payloadSize == sizeof(PresetPayload) &&
      (size_t)f.size() == sizeof(PresetHeader) + sizeof(PresetPayload)) {
    ok = f.read((uint8_t *)&p, sizeof(p)) == (int)sizeof(p);
    f.close();
    if (!ok || crc32Bytes((const uint8_t *)&p, sizeof(p)) != h.crc32) return false;
  } else if (h.version == PRESET_VERSION_V1 &&
             h.payloadSize == sizeof(PresetPayloadV1) &&
             (size_t)f.size() == sizeof(PresetHeader) + sizeof(PresetPayloadV1)) {
    PresetPayloadV1 old{};
    ok = f.read((uint8_t *)&old, sizeof(old)) == (int)sizeof(old);
    f.close();
    if (!ok || crc32Bytes((const uint8_t *)&old, sizeof(old)) != h.crc32) return false;

    memcpy(p.name, old.name, sizeof(old.name));
    memcpy(p.values, old.values, sizeof(old.values));
    // New V0.14B reverb defaults for legacy presets: safely OFF by default.
    p.values[P_REV_VOL] = miosaurusStateDef(P_REV_VOL).defaultValue;
    p.values[P_REV_SIZE] = miosaurusStateDef(P_REV_SIZE).defaultValue;
    p.values[P_REV_DECAY] = miosaurusStateDef(P_REV_DECAY).defaultValue;
    p.values[P_REV_DAMP] = miosaurusStateDef(P_REV_DAMP).defaultValue;
    p.legato = old.legato;
    p.priority = old.priority;
    p.para = old.para;
    p.velocityRoute = old.velocityRoute;
    p.aftertouchRoute = old.aftertouchRoute;
    p.velocityCurve = old.velocityCurve;
    p.reserved0 = old.reserved0;
    p.reserved1 = old.reserved1;
    p.velocityAmount = old.velocityAmount;
    p.aftertouchAmount = old.aftertouchAmount;
  } else {
    f.close();
    return false;
  }

  p.name[MIO_PRESET_NAME_MAX] = '\0';
  return true;
}

static void cacheSlotNoLock(uint8_t slot, bool exists, const PresetPayload *p = nullptr) {
  if (!validSlot(slot)) return;
  sSlotCacheValid[slot] = true;
  sSlotCacheExists[slot] = exists;
  if (exists && p) {
    sanitizeName(p->name, sSlotCacheName[slot], sizeof(sSlotCacheName[slot]));
  } else {
    strncpy(sSlotCacheName[slot], "EMPTY", sizeof(sSlotCacheName[slot]) - 1);
    sSlotCacheName[slot][sizeof(sSlotCacheName[slot]) - 1] = '\0';
  }
}

static void buildSlotCacheNoLock() {
  for (uint16_t i = 0; i < MIO_PRESET_SLOT_COUNT; ++i) {
    PresetHeader h{};
    PresetPayload p{};
    const bool ok = readPresetFile((uint8_t)i, h, p);
    cacheSlotNoLock((uint8_t)i, ok, ok ? &p : nullptr);
    // Startup only; yield periodically so watchdog/idle still run.
    if ((i & 7U) == 7U) vTaskDelay(1);
  }
}

static bool verifyFilePath(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  PresetHeader h{};
  bool ok = f.read((uint8_t *)&h, sizeof(h)) == (int)sizeof(h);
  if (!ok || h.magic != PRESET_MAGIC || h.headerSize != sizeof(PresetHeader)) {
    f.close();
    return false;
  }

  if (h.version == PRESET_VERSION && h.payloadSize == sizeof(PresetPayload) &&
      (size_t)f.size() == sizeof(PresetHeader) + sizeof(PresetPayload)) {
    PresetPayload p{};
    ok = f.read((uint8_t *)&p, sizeof(p)) == (int)sizeof(p);
    f.close();
    return ok && crc32Bytes((const uint8_t *)&p, sizeof(p)) == h.crc32;
  }
  if (h.version == PRESET_VERSION_V1 && h.payloadSize == sizeof(PresetPayloadV1) &&
      (size_t)f.size() == sizeof(PresetHeader) + sizeof(PresetPayloadV1)) {
    PresetPayloadV1 p{};
    ok = f.read((uint8_t *)&p, sizeof(p)) == (int)sizeof(p);
    f.close();
    return ok && crc32Bytes((const uint8_t *)&p, sizeof(p)) == h.crc32;
  }

  f.close();
  return false;
}

static bool writeLastSlot(uint8_t slot) {
  if (!ensureDirectories()) return false;

  LastSlotFile s{};
  s.magic = LAST_MAGIC;
  s.version = LAST_VERSION;
  s.slot = slot;
  s.reserved = 0;
  s.crc32 = crc32Bytes((const uint8_t *)&s, offsetof(LastSlotFile, crc32));

  SD.remove(MIO_LAST_FILE);
  File f = SD.open(MIO_LAST_FILE, FILE_WRITE);
  if (!f) return false;
  bool ok = f.write((const uint8_t *)&s, sizeof(s)) == sizeof(s);
  f.flush();
  f.close();
  return ok;
}

static void scheduleLastSlotWrite() {
  sLastSlotDirty = true;
  sLastSlotDirtySinceMs = millis();
}

static bool queueLastSlotWrite(uint8_t slot) {
  if (!sSaveJobMutex) return false;
  if (xSemaphoreTake(sSaveJobMutex, 0) != pdTRUE) return false;
  sLastSlotWriteJobPending = true;
  sLastSlotWriteJobSlot = slot;
  xSemaphoreGive(sSaveJobMutex);
  if (sSaveTaskHandle) xTaskNotifyGive(sSaveTaskHandle);
  return true;
}

static uint8_t readLastSlot() {
  if (!Board::sdReady()) return 0;

  File f = SD.open(MIO_LAST_FILE, FILE_READ);
  if (!f) return 0;
  LastSlotFile s{};
  bool ok = f.read((uint8_t *)&s, sizeof(s)) == (int)sizeof(s);
  f.close();
  if (!ok || s.magic != LAST_MAGIC || s.version != LAST_VERSION ||
      !validSlot(s.slot)) return 0;

  uint32_t crc = crc32Bytes((const uint8_t *)&s, offsetof(LastSlotFile, crc32));
  return crc == s.crc32 ? s.slot : 0;
}

static void rememberBaselineNoLock() {
  miosaurusStateSnapshotActual(sBaseline, PARAM_COUNT);
  miosaurusPerformanceSnapshotPreset(sBaselinePerf);
  sEdited = false;
}

static bool performanceDiffers(const MioPerformancePresetState &a,
                               const MioPerformancePresetState &b) {
  return a.legato != b.legato ||
         a.priority != b.priority ||
         a.para != b.para ||
         a.velocityRoute != b.velocityRoute ||
         a.aftertouchRoute != b.aftertouchRoute ||
         fabsf(a.velocityAmount - b.velocityAmount) > 0.0001f ||
         fabsf(a.aftertouchAmount - b.aftertouchAmount) > 0.0001f ||
         a.velocityCurve != b.velocityCurve;
}

static bool computeEditedNoLock() {
  float now[PARAM_COUNT];
  miosaurusStateSnapshotActual(now, PARAM_COUNT);

  for (uint8_t i = 0; i < PARAM_COUNT; ++i) {
    const bool discrete = miosaurusStateDef((MioParamId)i).discrete;
    if (discrete) {
      if ((now[i] < 0.5f) != (sBaseline[i] < 0.5f)) return true;
    } else if (fabsf(now[i] - sBaseline[i]) > 0.0001f) {
      return true;
    }
  }

  MioPerformancePresetState p;
  miosaurusPerformanceSnapshotPreset(p);
  return performanceDiffers(p, sBaselinePerf);
}

static void payloadFromCurrent(PresetPayload &p, const char *name) {
  memset(&p, 0, sizeof(p));
  sanitizeName(name, p.name, sizeof(p.name));
  miosaurusStateSnapshotActual(p.values, PARAM_COUNT);

  MioPerformancePresetState perf;
  miosaurusPerformanceSnapshotPreset(perf);

  p.legato = perf.legato ? 1 : 0;
  p.priority = perf.priority;
  p.para = perf.para ? 1 : 0;
  p.velocityRoute = perf.velocityRoute;
  p.aftertouchRoute = perf.aftertouchRoute;
  p.velocityCurve = perf.velocityCurve;
  p.velocityAmount = perf.velocityAmount;
  p.aftertouchAmount = perf.aftertouchAmount;
}

static void applyPayload(const PresetPayload &p) {
  MioPerformancePresetState perf{};
  perf.legato = p.legato != 0;
  perf.priority = p.priority;
  perf.para = p.para != 0;
  perf.velocityRoute = p.velocityRoute;
  perf.aftertouchRoute = p.aftertouchRoute;
  perf.velocityAmount = p.velocityAmount;
  perf.aftertouchAmount = p.aftertouchAmount;
  perf.velocityCurve = p.velocityCurve;

  miosaurusPerformanceResetForPresetLoad();
  miosaurusStateApplyPresetActual(p.values, PARAM_COUNT);
  miosaurusPerformanceApplyPreset(perf);
}

static bool loadNoLock(uint8_t slot, bool showStatus) {
  const uint32_t loadStartUs = micros();
  PresetHeader h{};
  PresetPayload p{};
  if (!readPresetFile(slot, h, p)) {
    ++sDiagLoadFail;
    const uint32_t us=(uint32_t)(micros()-loadStartUs);
    if(us>sDiagLoadUsMax) sDiagLoadUsMax=us;
    if (showStatus) setStatus("EMPTY/ERR");
    return false;
  }

  applyPayload(p);
  cacheSlotNoLock(slot, true, &p);

  sLoadedSlot = slot;
  sBrowseSlot = slot;
  sCurrentExists = true;
  sanitizeName(p.name, sLoadedName, sizeof(sLoadedName));
  rememberBaselineNoLock();
  scheduleLastSlotWrite();

  ++sDiagLoadOk;
  const uint32_t us=(uint32_t)(micros()-loadStartUs);
  if(us>sDiagLoadUsMax) sDiagLoadUsMax=us;
  if (showStatus) setStatus("LOAD");
  else bumpRevision();
  return true;
}

static bool writePresetPayloadNoLock(uint8_t slot, const PresetPayload &p) {
  if (!validSlot(slot) || !ensureDirectories()) {
    setStatus("SD ERR");
    return false;
  }

  PresetHeader h{};
  h.magic = PRESET_MAGIC;
  h.version = PRESET_VERSION;
  h.headerSize = sizeof(PresetHeader);
  h.payloadSize = sizeof(PresetPayload);
  h.crc32 = crc32Bytes((const uint8_t *)&p, sizeof(p));

  char finalPath[64], tempPath[64], backupPath[64];
  slotPath(slot, finalPath, sizeof(finalPath));
  tmpPath(slot, tempPath, sizeof(tempPath));
  bakPath(slot, backupPath, sizeof(backupPath));

  SD.remove(tempPath);
  File f = SD.open(tempPath, FILE_WRITE);
  if (!f) {
    setStatus("WRITE ERR");
    return false;
  }

  bool ok = f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);
  if (ok) ok = f.write((const uint8_t *)&p, sizeof(p)) == sizeof(p);
  f.flush();
  f.close();

  if (!ok || !verifyFilePath(tempPath)) {
    SD.remove(tempPath);
    setStatus("VERIFY ERR");
    return false;
  }

  SD.remove(backupPath);
  const bool hadFinal = SD.exists(finalPath);

  if (hadFinal && !SD.rename(finalPath, backupPath)) {
    SD.remove(tempPath);
    setStatus("BACKUP ERR");
    return false;
  }

  if (!SD.rename(tempPath, finalPath)) {
    if (hadFinal && SD.exists(backupPath)) {
      SD.rename(backupPath, finalPath);
    }
    SD.remove(tempPath);
    setStatus("RENAME ERR");
    return false;
  }

  if (!verifyFilePath(finalPath)) {
    SD.remove(finalPath);
    if (hadFinal && SD.exists(backupPath)) {
      SD.rename(backupPath, finalPath);
    }
    setStatus("CRC ERR");
    return false;
  }

  SD.remove(backupPath);
  cacheSlotNoLock(slot, true, &p);

  sLoadedSlot = slot;
  sBrowseSlot = slot;
  sCurrentExists = true;
  sanitizeName(p.name, sLoadedName, sizeof(sLoadedName));
  rememberBaselineNoLock();
  scheduleLastSlotWrite();
  setStatus("SAVE");
  return true;
}

static bool queueSaveJobNoLock(uint8_t slot, const char *name, bool saveAs) {
  if (!validSlot(slot)) return false;
  if (!sSaveJobMutex) return false;

  PresetPayload snap{};
  payloadFromCurrent(snap, name);

  if (xSemaphoreTake(sSaveJobMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    setStatus("SAVE BUSY");
    return false;
  }

  // Latest requested save replaces an older not-yet-started request.
  sSaveJob.pending = true;
  sSaveJob.saveAs = saveAs;
  sSaveJob.slot = slot;
  sSaveJob.payload = snap;
  xSemaphoreGive(sSaveJobMutex);

  setStatus("SAVE...", 3000);
  if (sSaveTaskHandle) xTaskNotifyGive(sSaveTaskHandle);
  return true;
}

static bool queueLoadJobNoLock(uint8_t slot, bool showStatus, bool programChange) {
  if (!validSlot(slot) || !sLoadJobMutex) return false;

  if (xSemaphoreTake(sLoadJobMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    if (showStatus) setStatus("LOAD BUSY");
    return false;
  }

  uint32_t id = sLatestLoadRequestId + 1U;
  if (id == 0) id = 1;
  sLatestLoadRequestId = id;

  sLoadJob.pending = true;
  sLoadJob.showStatus = showStatus;
  sLoadJob.programChange = programChange;
  sLoadJob.slot = slot;
  sLoadJob.requestId = id;
  xSemaphoreGive(sLoadJobMutex);

  if (showStatus) setStatus("LOAD...", 3000);
  if (sLoadTaskHandle) xTaskNotifyGive(sLoadTaskHandle);
  return true;
}

static void presetLoadWorker(void *arg) {
  (void)arg;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    for (;;) {
      LoadJob job{};
      bool haveJob = false;

      if (sLoadJobMutex &&
          xSemaphoreTake(sLoadJobMutex, portMAX_DELAY) == pdTRUE) {
        if (sLoadJob.pending) {
          job = sLoadJob;
          sLoadJob.pending = false;
          haveJob = true;
        }
        xSemaphoreGive(sLoadJobMutex);
      }
      if (!haveJob) break;

      // Let MioControl/MioGUI run first after the request gesture/message.
      vTaskDelay(pdMS_TO_TICKS(1));

      PresetHeader h{};
      PresetPayload payload{};
      const uint32_t loadStartUs = micros();
      bool ok = false;

      // Serialize SD/FAT against save and metadata startup operations.
      if (sMutex && xSemaphoreTake(sMutex, portMAX_DELAY) == pdTRUE) {
        ok = readPresetFile(job.slot, h, payload);
        xSemaphoreGive(sMutex);
      }
      const uint32_t loadUs = (uint32_t)(micros() - loadStartUs);

      if (loadUs > sDiagLoadUsMax) sDiagLoadUsMax = loadUs;

      if (sLoadJobMutex &&
          xSemaphoreTake(sLoadJobMutex, portMAX_DELAY) == pdTRUE) {
        // Only publish a result if it still represents the newest request.
        if (job.requestId == sLatestLoadRequestId) {
          sLoadResult.ready = true;
          sLoadResult.ok = ok;
          sLoadResult.showStatus = job.showStatus;
          sLoadResult.programChange = job.programChange;
          sLoadResult.slot = job.slot;
          sLoadResult.requestId = job.requestId;
          sLoadResult.loadUs = loadUs;
          if (ok) sLoadResult.payload = payload;
        }
        xSemaphoreGive(sLoadJobMutex);
      }

      vTaskDelay(1);
    }
  }
}

static bool consumeLoadResult() {
  if (!sLoadJobMutex || !sMutex) return false;

  LoadResult result{};
  bool have = false;
  if (xSemaphoreTake(sLoadJobMutex, 0) == pdTRUE) {
    if (sLoadResult.ready) {
      result = sLoadResult;
      sLoadResult.ready = false;
      have = true;
    }
    xSemaphoreGive(sLoadJobMutex);
  }
  if (!have) return false;

  // Newer request arrived after this result was produced: never apply stale
  // preset data (prevents intermediate patch flashes during fast browsing).
  if (result.requestId != sLatestLoadRequestId) return true;

  if (xSemaphoreTake(sMutex, 0) != pdTRUE) {
    // Save/other preset operation currently owns the layer. Put the result
    // back for the next control pass instead of blocking MioControl.
    if (xSemaphoreTake(sLoadJobMutex, 0) == pdTRUE) {
      sLoadResult = result;
      sLoadResult.ready = true;
      xSemaphoreGive(sLoadJobMutex);
    }
    return false;
  }

  if (!result.ok) {
    cacheSlotNoLock(result.slot, false, nullptr);
    ++sDiagLoadFail;
    if (result.showStatus) setStatus(result.programChange ? "PC EMPTY" : "EMPTY/ERR");
    xSemaphoreGive(sMutex);
    return true;
  }

  // Fast apply only: no SD/FAT work occurs in MioControl here.
  applyPayload(result.payload);
  cacheSlotNoLock(result.slot, true, &result.payload);
  sLoadedSlot = result.slot;
  sBrowseSlot = result.slot;
  sCurrentExists = true;
  sanitizeName(result.payload.name, sLoadedName, sizeof(sLoadedName));
  rememberBaselineNoLock();
  scheduleLastSlotWrite();
  ++sDiagLoadOk;
  if (result.showStatus) setStatus("LOAD");
  else bumpRevision();
  xSemaphoreGive(sMutex);
  return true;
}

static void presetSaveWorker(void *arg) {
  (void)arg;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    SaveJob job{};
    bool haveSaveJob = false;
    bool haveLastSlotJob = false;
    uint8_t lastSlot = 0;

    if (sSaveJobMutex &&
        xSemaphoreTake(sSaveJobMutex, portMAX_DELAY) == pdTRUE) {
      if (sSaveJob.pending) {
        job = sSaveJob;
        sSaveJob.pending = false;
        haveSaveJob = true;
      }
      if (sLastSlotWriteJobPending) {
        lastSlot = sLastSlotWriteJobSlot;
        sLastSlotWriteJobPending = false;
        haveLastSlotJob = true;
      }
      xSemaphoreGive(sSaveJobMutex);
    }

    if (!haveSaveJob && !haveLastSlotJob) continue;

    // Yield before touching the SD/FAT layer so MioControl/MioGUI get a
    // scheduling opportunity immediately after the event that queued this.
    vTaskDelay(pdMS_TO_TICKS(2));

    if (haveSaveJob) {
      bool ok = false;
      const uint32_t saveStartUs = micros();
      if (sMutex && xSemaphoreTake(sMutex, portMAX_DELAY) == pdTRUE) {
        ok = writePresetPayloadNoLock(job.slot, job.payload);
        xSemaphoreGive(sMutex);
      }
      const uint32_t saveUs=(uint32_t)(micros()-saveStartUs);
      if(saveUs>sDiagSaveUsMax) sDiagSaveUsMax=saveUs;
      if(ok) ++sDiagSaveOk; else ++sDiagSaveFail;
    }

    if (haveLastSlotJob) {
      bool ok = false;
      if (sMutex && xSemaphoreTake(sMutex, portMAX_DELAY) == pdTRUE) {
        ok = writeLastSlot(lastSlot);
        if (ok && sLoadedSlot == lastSlot) {
          sLastSlotDirty = false;
        } else if (!ok) {
          // Retry later without hammering the card.
          sLastSlotDirty = true;
          sLastSlotDirtySinceMs = millis();
        }
        xSemaphoreGive(sMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

} // namespace

bool miosaurusPresetBegin() {
  if (!sMutex) sMutex = xSemaphoreCreateMutex();
  if (!sSaveJobMutex) sSaveJobMutex = xSemaphoreCreateMutex();
  if (!sLoadJobMutex) sLoadJobMutex = xSemaphoreCreateMutex();
  if (!sMutex || !sSaveJobMutex || !sLoadJobMutex) return false;

  if (!sSaveTaskHandle) {
    BaseType_t taskOk = xTaskCreatePinnedToCore(
        presetSaveWorker,
        "MioPresetSave",
        4096,
        nullptr,
        1,              // deliberately lower than MioControl / GUI
        &sSaveTaskHandle,
        0);
    if (taskOk != pdPASS) {
      sSaveTaskHandle = nullptr;
      return false;
    }
  }

  if (!sLoadTaskHandle) {
    BaseType_t taskOk = xTaskCreatePinnedToCore(
        presetLoadWorker,
        "MioPresetLoad",
        4096,
        nullptr,
        1,              // SD/FAT must never outrank MioControl / GUI
        &sLoadTaskHandle,
        0);
    if (taskOk != pdPASS) {
      sLoadTaskHandle = nullptr;
      return false;
    }
  }

  xSemaphoreTake(sMutex, portMAX_DELAY);

  if (!Board::sdReady()) {
    sLoadedSlot = 0;
    sBrowseSlot = 0;
    sCurrentExists = false;
    rememberBaselineNoLock();
    setStatus("NO SD", 3000);
    xSemaphoreGive(sMutex);
    return true; // synth remains usable with current/default state
  }

  if (!ensureDirectories()) {
    sCurrentExists = false;
    rememberBaselineNoLock();
    setStatus("SD DIR ERR", 3000);
    xSemaphoreGive(sMutex);
    return true;
  }

  // Build runtime metadata cache before audio starts. This intentionally
  // trades a little startup time for zero GUI-triggered preset file opens.
  buildSlotCacheNoLock();

  sLoadedSlot = readLastSlot();
  sBrowseSlot = sLoadedSlot;

  if (!loadNoLock(sLoadedSlot, false)) {
    // Prefer P000, then first valid preset, otherwise keep current defaults.
    bool loaded = false;
    for (uint16_t i = 0; i < MIO_PRESET_SLOT_COUNT; ++i) {
      if (loadNoLock((uint8_t)i, false)) {
        loaded = true;
        break;
      }
    }
    if (!loaded) {
      sLoadedSlot = 0;
      sBrowseSlot = 0;
      sCurrentExists = false;
      strcpy(sLoadedName, "INIT PATCH");
      rememberBaselineNoLock();
      setStatus("EMPTY");
    } else {
      setStatus("LOAD");
    }
  } else {
    setStatus("LOAD");
  }

  xSemaphoreGive(sMutex);
  return true;
}

void miosaurusPresetService() {
  const uint32_t now = millis();

  // Apply completed worker result first. This step is RAM-only and bounded.
  consumeLoadResult();

  // 1) Coalesced MIDI Program Change.
  // Only after MIDI has been quiet briefly do we touch the SD card.
  bool doProgramLoad = false;
  uint8_t programToLoad = 0;

  if (sProgramPending &&
      (uint32_t)(now - sProgramRequestMs) >= PROGRAM_CHANGE_QUIET_MS) {
    // Snapshot latest request. A newer PC arriving during the SD read will
    // simply remain pending for the next service pass.
    programToLoad = sPendingProgram;
    sProgramPending = false;
    doProgramLoad = true;
  }

  if (doProgramLoad && sMutex) {
    if (xSemaphoreTake(sMutex, 0) == pdTRUE) {
      sBrowseSlot = programToLoad;
      ++sDiagPcLoads;
      const bool queued = queueLoadJobNoLock(programToLoad, true, true);
      xSemaphoreGive(sMutex);
      if (!queued) {
        sPendingProgram = programToLoad;
        sProgramRequestMs = now;
        sProgramPending = true;
      }
    } else {
      // Retry without ever blocking MioControl on preset ownership.
      sPendingProgram = programToLoad;
      sProgramRequestMs = now;
      sProgramPending = true;
    }
  }

  // 2) Edited-state/status housekeeping.
  if ((uint32_t)(now - sLastEditedCheckMs) >= 25) {
    sLastEditedCheckMs = now;

    if (sMutex && xSemaphoreTake(sMutex, 0) == pdTRUE) {
      bool editedNow = computeEditedNoLock();
      if (editedNow != sEdited) {
        sEdited = editedNow;
        bumpRevision();
      }

      if (sStatusUntilMs && (int32_t)(now - sStatusUntilMs) >= 0) {
        sStatusUntilMs = 0;
        sStatus[0] = '\0';
        bumpRevision();
      }

      // 3) Persist only after the selection has remained stable.
      // Never do this while another MIDI Program Change is pending.
      if (sLastSlotDirty &&
          !sProgramPending &&
          (uint32_t)(now - sLastSlotDirtySinceMs) >= LAST_SLOT_PERSIST_DELAY_MS) {
        const uint8_t stableSlot = sLoadedSlot;
        if (queueLastSlotWrite(stableSlot)) {
          // Mark as queued. Worker will clear permanently on successful write;
          // a later preset load will set dirty again with a fresh timestamp.
          sLastSlotDirty = false;
        } else {
          sLastSlotDirtySinceMs = now;
        }
      }

      xSemaphoreGive(sMutex);
    }
  }
}

uint8_t miosaurusPresetLoadedSlot() { return sLoadedSlot; }
uint8_t miosaurusPresetBrowseSlot() { return sBrowseSlot; }
bool miosaurusPresetEdited() { return sEdited; }
bool miosaurusPresetCurrentExists() { return sCurrentExists; }
uint32_t miosaurusPresetRevision() { return sRevision; }
const char *miosaurusPresetLoadedName() { return sLoadedName; }
const char *miosaurusPresetStatus() { return sStatus; }

bool miosaurusPresetSlotExists(uint8_t slot) {
  if (!validSlot(slot) || !sSlotCacheValid[slot]) return false;
  return sSlotCacheExists[slot];
}

bool miosaurusPresetGetSlotName(uint8_t slot, char *out, size_t outLen) {
  if (!out || outLen == 0) return false;
  out[0] = '\0';

  if (!validSlot(slot) || !sSlotCacheValid[slot] || !sSlotCacheExists[slot]) {
    strncpy(out, "EMPTY", outLen - 1);
    out[outLen - 1] = '\0';
    return false;
  }

  strncpy(out, sSlotCacheName[slot], outLen - 1);
  out[outLen - 1] = '\0';
  return true;
}

void miosaurusPresetSetBrowseSlot(uint8_t slot) {
  if (!validSlot(slot) || !sMutex) return;
  xSemaphoreTake(sMutex, portMAX_DELAY);
  if (sBrowseSlot != slot) {
    sBrowseSlot = slot;
    setStatus("SEL", 700);
  }
  xSemaphoreGive(sMutex);
}

void miosaurusPresetBrowsePrev() {
  uint8_t slot = (uint8_t)((sBrowseSlot + MIO_PRESET_SLOT_COUNT - 1)
                         % MIO_PRESET_SLOT_COUNT);
  miosaurusPresetSetBrowseSlot(slot);
}

void miosaurusPresetBrowseNext() {
  uint8_t slot = (uint8_t)((sBrowseSlot + 1) % MIO_PRESET_SLOT_COUNT);
  miosaurusPresetSetBrowseSlot(slot);
}

bool miosaurusPresetLoadBrowse() {
  if (!sMutex) return false;
  if (xSemaphoreTake(sMutex, pdMS_TO_TICKS(5)) != pdTRUE) return false;
  const uint8_t slot = sBrowseSlot;
  const bool ok = queueLoadJobNoLock(slot, true, false);
  xSemaphoreGive(sMutex);
  return ok;
}

bool miosaurusPresetSaveLoaded(const char *name) {
  if (!sMutex) return false;

  xSemaphoreTake(sMutex, portMAX_DELAY);
  const uint8_t slot = sLoadedSlot;
  bool ok = queueSaveJobNoLock(slot, name, false);
  xSemaphoreGive(sMutex);
  return ok;
}

bool miosaurusPresetSaveAsBrowse(const char *name) {
  if (!sMutex) return false;

  xSemaphoreTake(sMutex, portMAX_DELAY);
  const uint8_t slot = sBrowseSlot;
  bool ok = queueSaveJobNoLock(slot, name, true);
  xSemaphoreGive(sMutex);
  return ok;
}

bool miosaurusPresetLoadProgram(uint8_t programNumber) {
  if (!validSlot(programNumber)) return false;
  ++sDiagPcRequests;

  // Non-blocking by design: MIDI parser only records the newest target.
  // Multiple incoming PCs collapse into one final SD load.
  sPendingProgram = programNumber;
  sProgramRequestMs = millis();
  sProgramPending = true;
  return true;
}

void miosaurusPresetGetDiagnostics(MioPresetDiagnostics &out) {
  out.pcRequests = sDiagPcRequests;
  out.pcLoads = sDiagPcLoads;
  out.loadOk = sDiagLoadOk;
  out.loadFail = sDiagLoadFail;
  out.loadUsMax = sDiagLoadUsMax;
  out.saveOk = sDiagSaveOk;
  out.saveFail = sDiagSaveFail;
  out.saveUsMax = sDiagSaveUsMax;
}
