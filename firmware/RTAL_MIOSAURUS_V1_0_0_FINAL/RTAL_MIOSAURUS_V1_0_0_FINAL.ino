// RTAL MIOSAURUS V1.0.0 FINAL
// Hardware-confirmed basis: V0.9
//
// JC4827W543 platform:
//   JC4827W543 / NV3041A / GT911 / LVGL 8.4
//   Audio: 32 kHz / 256 frames
//   I2S: BCLK16 / LRCK15 / DOUT7
//   DIN MIDI: RX17 / TX18
//   USB MIDI Device: native ESP32-S3 USB-OTG/TinyUSB
//
// V0.10 adds the z33 performance/MIDI layer:
//   Mono LAST/LOW/HIGH, Legato, Para2, note-off coalescing,
//   Pitch Bend, Modwheel, Channel Aftertouch,
//   Velocity/Aftertouch routing + velocity curve,
//   CC Bank0/Bank1, MIDI-learn mapping infrastructure,
//   MIDI Clock/Start/Stop with LFO/Delay sync.
//
// V0.11 adds the z33-compatible 8-slot preset and Program Change layer.

#include <Arduino.h>
#include <lvgl.h>
#include "Board.h"
#include "MiosaurusSplash.h"
#include "Z_MiosaurusGui.h"
#include "Z_MiosaurusState.h"
#include "Z_MiosaurusAudio.h"
#include "Z_MiosaurusMidi.h"
#include "Z_MiosaurusPerformance.h"
#include "Z_MiosaurusPreset.h"

static TaskHandle_t guiTaskHandle = nullptr;
static TaskHandle_t controlTaskHandle = nullptr;
static TaskHandle_t audioTaskHandle = nullptr;


static void guiTask(void *) {
  for (;;) {
    Board::serviceLVGL();
    miosaurusGuiService();
    vTaskDelay(1);
  }
}

static void controlTask(void *) {
  for (;;) {
    miosaurusStateControlService();
    miosaurusMidiService();
    miosaurusPerformanceService();
    miosaurusPresetService();
    vTaskDelay(1);
  }
}

void setup() {
  if (!Board::begin()) {
    
    abort();
  }

  // V0.14B FIX4: selected RTAL MIOSAURUS splash, embedded as RGB565
  // PROGMEM.  No SD access and no framebuffer-sized RAM allocation.  Existing
  // state/preset initialization runs while the splash is visible, so only the
  // remainder of the 1.7 s minimum dwell adds to boot time.
  const uint32_t splashStartMs = millis();
  if (!Board::showSplashRGB565(MIOSAURUS_SPLASH_RGB565,
                               MIOSAURUS_SPLASH_W, MIOSAURUS_SPLASH_H)) {
    
  }

  if (!miosaurusStateBegin()) {
    
    abort();
  }
  if (!miosaurusPerformanceBegin()) {
    
    abort();
  }
  if (!miosaurusPresetBegin()) {
    
    abort();
  }

  // Keep the splash visible for about 1.7 s total.  Initialization time above
  // already counts toward this duration.
  while ((uint32_t)(millis() - splashStartMs) < 1700U) {
    delay(5);
  }

  // Any touch made on the splash must not land on MAIN after the screen swap.
  // LVGL will continue to report RELEASED until GT911 has been physically
  // released and stable for 120 ms.
  Board::suppressTouchUntilReleased(120);

  lv_obj_t *mainScreen = lv_obj_create(nullptr);
  lv_obj_clear_flag(mainScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(mainScreen, 0, 0);
  lv_obj_set_style_border_width(mainScreen, 0, 0);
  lv_scr_load(mainScreen);

  miosaurusGuiCreate(mainScreen);
  miosaurusStateMarkAllGuiDirty();

  for (int i = 0; i < 8; ++i) {
    Board::serviceLVGL();
    miosaurusGuiService();
    delay(5);
  }

  if (!miosaurusMidiBegin()) {
    
    abort();
  }
  if (!miosaurusAudioBegin()) {
    
    abort();
  }

  if (xTaskCreatePinnedToCore(
      controlTask, "MioControl", 8192, nullptr, 5, &controlTaskHandle, 0) != pdPASS) {
    
    abort();
  }

  if (xTaskCreatePinnedToCore(
      guiTask, "MioGUI", 16384, nullptr, 4, &guiTaskHandle, 0) != pdPASS) {
    
    abort();
  }

  if (xTaskCreatePinnedToCore(
      miosaurusAudioTask, "MioAudio", 16384, nullptr,
      configMAX_PRIORITIES - 1, &audioTaskHandle, 1) != pdPASS) {
    
    abort();
  }


  
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
