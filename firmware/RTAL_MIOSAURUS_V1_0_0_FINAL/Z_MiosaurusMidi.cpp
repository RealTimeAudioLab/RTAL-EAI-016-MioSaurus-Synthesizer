/*
  RTAL MIOSAURUS V0.12 FIX10
  Parallel DIN MIDI + native USB MIDI Device input.

  DIN MIDI:
    UART1 RX17 / TX18, 31250 baud

  USB MIDI:
    ESP32-S3 native USB peripheral, Arduino-ESP32 USBMIDI class.
    Requires Tools -> USB Mode -> USB-OTG (TinyUSB).

  Both transports feed the exact same dispatch layer. Parser state is kept
  separate per transport so simultaneous DIN + USB traffic cannot corrupt
  running status.
*/

#include "Z_MiosaurusMidi.h"
#include "Z_MiosaurusPerformance.h"
#include "Z_MiosaurusPreset.h"
#include "Board.h"
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#if defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 0)
  SET_USB_MIDI_DEVICE_NAME("RTAL MIOSAURUS")
  #include "USB.h"
  #include "USBMIDI.h"
  #define MIO_USB_MIDI_AVAILABLE 1
#else
  #define MIO_USB_MIDI_AVAILABLE 0
#endif

namespace {

static HardwareSerial sMidiDin(1);
static constexpr int MIDI_RX_PIN = 17;
static constexpr int MIDI_TX_PIN = 18;
static uint8_t sMidiReceiveChannel = 1; // 0=OMNI, 1..16 fixed

static constexpr const char *MIDI_CONFIG_FILE = "/RTAL_MIOSAURUS/MIDI_CFG.bin";
static constexpr uint32_t MIDI_CONFIG_MAGIC = 0x4D494346UL; // "MICF"
static constexpr uint16_t MIDI_CONFIG_VERSION = 1;

// V0.13C FIX1: Persist receive-channel changes outside the GUI/control path.
// The last selected channel wins; short repeated taps are coalesced.
static TaskHandle_t sMidiConfigTaskHandle = nullptr;
static SemaphoreHandle_t sMidiConfigMutex = nullptr;
static volatile bool sMidiConfigPending = false;
static volatile uint8_t sMidiConfigPendingChannel = 1;

struct __attribute__((packed)) MidiConfigFile {
  uint32_t magic;
  uint16_t version;
  uint8_t receiveChannel;
  uint8_t reserved;
  uint32_t crc32;
};

static uint32_t configCrc(const uint8_t *data, size_t len) {
  uint32_t crc=0xFFFFFFFFUL;
  for(size_t i=0;i<len;++i) {
    crc^=data[i];
    for(uint8_t b=0;b<8;++b)
      crc=(crc>>1)^(0xEDB88320UL & (uint32_t)-(int32_t)(crc&1U));
  }
  return ~crc;
}

static void writeMidiConfig(uint8_t channel) {
  if(!Board::sdReady()) return;
  if(!SD.exists("/RTAL_MIOSAURUS")) SD.mkdir("/RTAL_MIOSAURUS");

  MidiConfigFile f{};
  f.magic=MIDI_CONFIG_MAGIC;
  f.version=MIDI_CONFIG_VERSION;
  f.receiveChannel=channel;
  f.crc32=configCrc((const uint8_t*)&f,offsetof(MidiConfigFile,crc32));

  const char *tmp="/RTAL_MIOSAURUS/MIDI_CFG.tmp";
  SD.remove(tmp);
  File out=SD.open(tmp,FILE_WRITE);
  if(!out) return;
  const bool ok=out.write((const uint8_t*)&f,sizeof(f))==sizeof(f);
  out.flush();
  out.close();
  if(!ok) { SD.remove(tmp); return; }

  SD.remove(MIDI_CONFIG_FILE);
  SD.rename(tmp,MIDI_CONFIG_FILE);
}

static void midiConfigWorker(void *arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Coalesce rapid channel cycling and keep SD/FAT outside the touch callback.
    vTaskDelay(pdMS_TO_TICKS(250));

    uint8_t channel = 1;
    bool have = false;
    if (sMidiConfigMutex && xSemaphoreTake(sMidiConfigMutex, portMAX_DELAY) == pdTRUE) {
      if (sMidiConfigPending) {
        channel = sMidiConfigPendingChannel;
        sMidiConfigPending = false;
        have = true;
      }
      xSemaphoreGive(sMidiConfigMutex);
    }
    if (have) writeMidiConfig(channel);
    vTaskDelay(1);
  }
}

static void queueMidiConfigSave(uint8_t channel) {
  if (!sMidiConfigMutex) return;
  if (xSemaphoreTake(sMidiConfigMutex, pdMS_TO_TICKS(2)) != pdTRUE) return;
  sMidiConfigPendingChannel = channel;
  sMidiConfigPending = true;
  xSemaphoreGive(sMidiConfigMutex);
  if (sMidiConfigTaskHandle) xTaskNotifyGive(sMidiConfigTaskHandle);
}

static void loadMidiConfig() {
  sMidiReceiveChannel=1;
  if(!Board::sdReady()) return;

  File in=SD.open(MIDI_CONFIG_FILE,FILE_READ);
  if(!in) return;

  MidiConfigFile f{};
  const bool ok=in.read((uint8_t*)&f,sizeof(f))==(int)sizeof(f);
  in.close();
  if(!ok || f.magic!=MIDI_CONFIG_MAGIC ||
     f.version!=MIDI_CONFIG_VERSION) return;

  const uint32_t crc=configCrc((const uint8_t*)&f,offsetof(MidiConfigFile,crc32));
  if(crc!=f.crc32) return;
  if(f.receiveChannel<=16) sMidiReceiveChannel=f.receiveChannel;
}

#if MIO_USB_MIDI_AVAILABLE
static USBMIDI sMidiUsb;
static bool sUsbMidiStarted = false;
#endif

struct MidiParserState {
  uint8_t runningStatus = 0;
  uint8_t data1 = 0;
  uint8_t dataCount = 0;
};

static MidiParserState sDinParser{};
static MidiParserState sUsbParser{};

static volatile uint32_t sDiagDinBytes = 0;
static volatile uint32_t sDiagUsbPackets = 0;
static volatile uint32_t sDiagUsbMidiBytes = 0;
static volatile uint32_t sDiagDinMessages = 0;
static volatile uint32_t sDiagUsbMessages = 0;
static volatile uint32_t sDiagNoteOn = 0;
static volatile uint32_t sDiagNoteOff = 0;
static volatile uint32_t sDiagCc = 0;
static volatile uint32_t sDiagPc = 0;
static volatile uint32_t sDiagAftertouch = 0;
static volatile uint32_t sDiagPitchBend = 0;
static volatile uint32_t sDiagClock = 0;
static volatile uint32_t sDiagStart = 0;
static volatile uint32_t sDiagStop = 0;

static uint8_t needed(uint8_t status) {
  const uint8_t k = status & 0xF0;
  if (k == 0xC0 || k == 0xD0) return 1;
  if (k >= 0x80 && k <= 0xE0) return 2;
  return 0;
}

static void dispatch(bool usb, uint8_t status, uint8_t a, uint8_t b) {
  const uint8_t k = status & 0xF0;
  if (sMidiReceiveChannel != 0) {
    const uint8_t msgChannel = (status & 0x0F) + 1;
    if (msgChannel != sMidiReceiveChannel) return;
  }

  if (usb) ++sDiagUsbMessages; else ++sDiagDinMessages;

  switch (k) {
    case 0x80:
      ++sDiagNoteOff;
      miosaurusPerformanceNoteOff(a);
      break;

    case 0x90:
      if (b == 0) { ++sDiagNoteOff; miosaurusPerformanceNoteOff(a); }
      else { ++sDiagNoteOn; miosaurusPerformanceNoteOn(a, b); }
      break;

    case 0xB0:
      ++sDiagCc;
      miosaurusMidiApplyCC(a, b);
      break;

    case 0xC0:
      ++sDiagPc;
      // FIX4 preset layer coalesces dense Program Change streams.
      miosaurusPresetLoadProgram(a);
      break;

    case 0xD0:
      ++sDiagAftertouch;
      miosaurusPerformanceAftertouch(a);
      break;

    case 0xE0: {
      ++sDiagPitchBend;
      const int raw14 = ((int)b << 7) | a;
      const int16_t bend = (int16_t)(raw14 - 8192);
      miosaurusPerformancePitchBend(bend);
      break;
    }

    default:
      break;
  }
}

static void feed(bool usb, MidiParserState &p, uint8_t c) {
  // MIDI realtime may appear anywhere and does not cancel running status.
  if (c >= 0xF8) {
    switch (c) {
      case 0xF8: ++sDiagClock; miosaurusPerformanceClock(); break;
      case 0xFA: ++sDiagStart; miosaurusPerformanceStart(); break;
      case 0xFC: ++sDiagStop; miosaurusPerformanceStop(); break;
      default: break;
    }
    return;
  }

  if (c & 0x80) {
    if (c < 0xF0) {
      p.runningStatus = c;
      p.dataCount = 0;
    } else {
      // SysEx/system-common is intentionally ignored by the current synth
      // parser, just as in the DIN-only FIX9 implementation.
      p.runningStatus = 0;
      p.dataCount = 0;
    }
    return;
  }

  if (!p.runningStatus) return;

  const uint8_t n = needed(p.runningStatus);
  if (!n) return;

  if (!p.dataCount) {
    p.data1 = c;
    p.dataCount = 1;

    if (n == 1) {
      dispatch(usb, p.runningStatus, p.data1, 0);
      p.dataCount = 0;
    }
  } else {
    dispatch(usb, p.runningStatus, p.data1, c);
    p.dataCount = 0;
  }
}

static void serviceDinMidi() {
  // Bounded work per service pass prevents MIDI floods from starving CPU0.
  int budget = 96;
  while (budget-- > 0 && sMidiDin.available() > 0) {
    ++sDiagDinBytes;
    feed(false, sDinParser, (uint8_t)sMidiDin.read());
  }
}

#if MIO_USB_MIDI_AVAILABLE
static void serviceUsbMidi() {
  if (!sUsbMidiStarted) return;

  // USB MIDI 1.0 CIN -> number of MIDI bytes in packet.
  static const int8_t cinSize[16] = {
    -1, -1, 2, 3, 3, 1, 2, 3,
     3,  3, 3, 3, 2, 2, 3, 1
  };

  // Also bounded so a DAW/controller flood cannot monopolize MioControl.
  int packetBudget = 32;
  midiEventPacket_t packet = {0, 0, 0, 0};

  while (packetBudget-- > 0 && sMidiUsb.readPacket(&packet)) {
    ++sDiagUsbPackets;
    const midi_code_index_number_t cin = MIDI_EP_HEADER_CIN_GET(packet.header);
    const int8_t size = cinSize[(uint8_t)cin & 0x0F];
    if (size <= 0) continue;

    const uint8_t *raw = (const uint8_t *)&packet;
    sDiagUsbMidiBytes += (uint32_t)size;
    for (int i = 0; i < size; ++i) {
      feed(true, sUsbParser, raw[i + 1]);
    }
  }
}
#endif

} // namespace

bool miosaurusMidiBegin() {
  if (!sMidiConfigMutex) sMidiConfigMutex = xSemaphoreCreateMutex();
  if (!sMidiConfigMutex) return false;
  if (!sMidiConfigTaskHandle) {
    BaseType_t taskOk = xTaskCreatePinnedToCore(
        midiConfigWorker, "MioMidiCfg", 3072, nullptr, 1,
        &sMidiConfigTaskHandle, 0);
    if (taskOk != pdPASS) {
      sMidiConfigTaskHandle = nullptr;
      return false;
    }
  }

  loadMidiConfig();
  sMidiDin.begin(31250, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);
  sDinParser = MidiParserState{};
  sUsbParser = MidiParserState{};

#if MIO_USB_MIDI_AVAILABLE
  // Arduino-ESP32 official USB MIDI sequence.
  sMidiUsb.begin();
  USB.begin();
  sUsbMidiStarted = true;
  
#else
  
  
#endif

  return true;
}

void miosaurusMidiService() {
  serviceDinMidi();

#if MIO_USB_MIDI_AVAILABLE
  serviceUsbMidi();
#endif
}


uint8_t miosaurusMidiReceiveChannel() {
  return sMidiReceiveChannel;
}

void miosaurusMidiSetReceiveChannel(uint8_t channel) {
  if(channel > 16) channel = 0;
  if(channel == sMidiReceiveChannel) return;

  // V0.13C FIX1: A note can be started on the old receive channel and its
  // later Note-Off would be filtered after the channel change. Release the
  // complete internal held-note/voice state before changing the filter.
  miosaurusPerformancePanic();

  // Also discard any half-received running-status message from the old
  // channel/transport so it cannot straddle the channel switch.
  sDinParser = MidiParserState{};
  sUsbParser = MidiParserState{};

  sMidiReceiveChannel = channel;
  queueMidiConfigSave(channel);
}

const char *miosaurusMidiReceiveChannelName() {
  static char text[8];
  if(sMidiReceiveChannel == 0) {
    snprintf(text,sizeof(text),"OMNI");
  } else {
    snprintf(text,sizeof(text),"CH %02u",(unsigned)sMidiReceiveChannel);
  }
  return text;
}

bool miosaurusUsbMidiAvailable() {
#if MIO_USB_MIDI_AVAILABLE
  return sUsbMidiStarted;
#else
  return false;
#endif
}

void miosaurusMidiGetDiagnostics(MioMidiDiagnostics &out) {
  out.dinBytes = sDiagDinBytes;
  out.usbPackets = sDiagUsbPackets;
  out.usbMidiBytes = sDiagUsbMidiBytes;
  out.dinMessages = sDiagDinMessages;
  out.usbMessages = sDiagUsbMessages;
  out.noteOn = sDiagNoteOn;
  out.noteOff = sDiagNoteOff;
  out.cc = sDiagCc;
  out.programChange = sDiagPc;
  out.aftertouch = sDiagAftertouch;
  out.pitchBend = sDiagPitchBend;
  out.clock = sDiagClock;
  out.start = sDiagStart;
  out.stop = sDiagStop;
}
