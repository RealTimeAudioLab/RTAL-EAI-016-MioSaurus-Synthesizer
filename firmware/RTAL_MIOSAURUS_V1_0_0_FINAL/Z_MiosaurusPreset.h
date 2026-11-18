#pragma once
#include <Arduino.h>

static constexpr uint16_t MIO_PRESET_SLOT_COUNT = 128;
static constexpr size_t MIO_PRESET_NAME_MAX = 31;

bool miosaurusPresetBegin();
void miosaurusPresetService();

uint8_t miosaurusPresetLoadedSlot();
uint8_t miosaurusPresetBrowseSlot();
bool miosaurusPresetEdited();
bool miosaurusPresetCurrentExists();
bool miosaurusPresetSlotExists(uint8_t slot);

void miosaurusPresetBrowsePrev();
void miosaurusPresetBrowseNext();
void miosaurusPresetSetBrowseSlot(uint8_t slot);

bool miosaurusPresetLoadBrowse();
bool miosaurusPresetSaveLoaded(const char *name);
bool miosaurusPresetSaveAsBrowse(const char *name);
bool miosaurusPresetLoadProgram(uint8_t programNumber);

bool miosaurusPresetGetSlotName(uint8_t slot, char *out, size_t outLen);
const char *miosaurusPresetLoadedName();

const char *miosaurusPresetStatus();
uint32_t miosaurusPresetRevision();

struct MioPresetDiagnostics {
  uint32_t pcRequests;
  uint32_t pcLoads;
  uint32_t loadOk;
  uint32_t loadFail;
  uint32_t loadUsMax;
  uint32_t saveOk;
  uint32_t saveFail;
  uint32_t saveUsMax;
};
void miosaurusPresetGetDiagnostics(MioPresetDiagnostics &out);
