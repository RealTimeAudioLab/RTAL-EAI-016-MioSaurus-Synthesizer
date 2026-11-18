#pragma once
#include <Arduino.h>

bool miosaurusMidiBegin();
void miosaurusMidiService();

// True when this build was compiled in ESP32-S3 USB-OTG/TinyUSB mode
// and the native USB MIDI device was started.
bool miosaurusUsbMidiAvailable();

struct MioMidiDiagnostics {
  uint32_t dinBytes;
  uint32_t usbPackets;
  uint32_t usbMidiBytes;
  uint32_t dinMessages;
  uint32_t usbMessages;
  uint32_t noteOn;
  uint32_t noteOff;
  uint32_t cc;
  uint32_t programChange;
  uint32_t aftertouch;
  uint32_t pitchBend;
  uint32_t clock;
  uint32_t start;
  uint32_t stop;
};
void miosaurusMidiGetDiagnostics(MioMidiDiagnostics &out);


// Global MIDI receive channel.
// 0 = OMNI, 1..16 = fixed MIDI channel.
uint8_t miosaurusMidiReceiveChannel();
void miosaurusMidiSetReceiveChannel(uint8_t channel);
const char *miosaurusMidiReceiveChannelName();
