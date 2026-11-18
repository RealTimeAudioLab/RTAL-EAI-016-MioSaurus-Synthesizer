#pragma once
#include <Arduino.h>

bool miosaurusAudioBegin();
void miosaurusAudioTask(void *);

void miosaurusAudioSetMonoNote(uint8_t note, uint8_t velocity, bool retrigger);
void miosaurusAudioSetParaNotes(int8_t note1, int8_t note2, int8_t subNote,
                                uint8_t vel1, uint8_t vel2,
                                bool retrigger, bool singleUnison);
void miosaurusAudioReleaseAll();
void miosaurusAudioResetLfo();
void miosaurusAudioResetFx();

// V0.13 validation diagnostics (read-only counters)
struct MioAudioDiagnostics {
  uint32_t blocks;
  uint32_t renderUsTotal;
  uint32_t renderUsMax;
  uint32_t renderOver8ms;
  uint32_t i2sErrors;
  uint32_t shortWrites;
  uint32_t voiceQueueDrops;
};
void miosaurusAudioGetDiagnostics(MioAudioDiagnostics &out);
