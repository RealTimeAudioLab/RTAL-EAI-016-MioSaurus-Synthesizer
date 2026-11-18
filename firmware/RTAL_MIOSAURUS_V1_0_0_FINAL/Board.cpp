#include "Board.h"
#include "PINS_JC4827W543.h"

#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <SD.h>
#include <TAMC_GT911.h>
#include "esp_heap_caps.h"

static SPIClass sSdSPI(HSPI);

static Arduino_DataBus *sBus = nullptr;
static Arduino_GFX *sGfx = nullptr;
static Arduino_NV3041A *sPanel = nullptr;

static TAMC_GT911 sTouch(
    TOUCH_SDA, TOUCH_SCL,
    TOUCH_INT, TOUCH_RES,
    LCD_WIDTH, LCD_HEIGHT
);

// LVGL 8.4 display/input objects.  These driver structs must remain alive
// for the complete lifetime of LVGL, therefore they are static.
static lv_disp_draw_buf_t sDrawBufDesc;
static lv_disp_drv_t sDispDrv;
static lv_disp_t *sDisplay = nullptr;
static lv_indev_drv_t sIndevDrv;
static lv_indev_t *sIndev = nullptr;
static lv_color_t *sDrawBuf = nullptr;

// FIX7U: Core0-only touch diagnostic instrumentation. No change to audio task.
static volatile uint8_t sTouchDiagMode = 0;
static uint32_t sTouchReadCount = 0;
static uint32_t sTouchReadTotalUs = 0;
static uint32_t sTouchReadMaxUs = 0;
static uint32_t sTouchPressedReads = 0;
static uint32_t sTouchCBCount = 0;
static uint32_t sTouchDiagLastPrintMs = 0;
static bool sSDReady = false;
static volatile bool sTouchPressed = false;
static volatile int16_t sTouchX = -1, sTouchY = -1;
static volatile uint32_t sTouchLastChangeMs = 0;
// V0.13C FIX2: modal-close touch quarantine.  A CLOSE release can be
// followed by a short GT911 re-press/ghost sample at the same coordinate.
// Keep LVGL released until the physical panel has remained released for a
// small settling window; this is deliberately local/opt-in, not a global
// debounce change.
static volatile bool sTouchSuppressUntilRelease = false;
static volatile uint32_t sTouchSuppressReleaseSinceMs = 0;
static volatile uint32_t sTouchSuppressSettleMs = 90;
static volatile uint8_t sDisplayTestMode = 0;
// FIX7Z: test LVGL handler cadence independently of the audio engine.
static volatile uint32_t sLvglIntervalMs7Z = 5;
static volatile bool sLvglHandlerPaused8A = false;
static volatile uint32_t sLvglHandlerCalls7Z = 0;
static volatile uint32_t sLvglHandlerMaxUs7Z = 0;
static volatile uint32_t sLvglHandlerOver3ms7Z = 0;
static volatile uint32_t sSkippedFlushes7Y = 0;
static volatile uint64_t sSkippedPixels7Y = 0;
static volatile uint32_t sGuiFrames = 0;
static volatile uint32_t sGuiFlushes = 0;
static volatile uint64_t sGuiPixels = 0;

// FIX4 comparison path: Guition Arduino_GFX RGB565 bitmap flush; 60-line LVGL buffer retained.
static constexpr uint16_t LVGL_BUF_LINES = 60;
static constexpr size_t LVGL_BUF_PIXELS = LCD_WIDTH * LVGL_BUF_LINES;
static constexpr size_t LVGL_BUF_BYTES = LVGL_BUF_PIXELS * sizeof(lv_color_t);

static void flushCB(
    lv_disp_drv_t *disp,
    const lv_area_t *area,
    lv_color_t *color_p)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    const uint32_t pixels = (uint32_t)w * (uint32_t)h;
    sGuiFlushes++;
    sGuiPixels += pixels;
    if(sDisplayTestMode != 0) {
        ++sSkippedFlushes7Y;
        sSkippedPixels7Y += pixels;
        if(lv_disp_flush_is_last(disp)) sGuiFrames++;
        lv_disp_flush_ready(disp);
        return;
    }

    // FIX4: Guition JC4827W543 LvglWidgets reference flush path.
    // The Arduino_GFX driver handles the NV3041A address window and transfer.
#if defined(LV_COLOR_16_SWAP) && (LV_COLOR_16_SWAP != 0)
    sGfx->draw16bitBeRGBBitmap(area->x1, area->y1,
                               reinterpret_cast<uint16_t *>(&color_p->full),
                               (uint16_t)w, (uint16_t)h);
#else
    sGfx->draw16bitRGBBitmap(area->x1, area->y1,
                             reinterpret_cast<uint16_t *>(&color_p->full),
                             (uint16_t)w, (uint16_t)h);
#endif
    if(lv_disp_flush_is_last(disp)) sGuiFrames++;
    lv_disp_flush_ready(disp);
}

static void touchCB(
    lv_indev_drv_t *indev_drv,
    lv_indev_data_t *data)
{
    (void)indev_drv;

    ++sTouchCBCount;
    if (sTouchDiagMode == 1) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    const uint32_t touchReadStart = micros();
    sTouch.read();
    const uint32_t touchReadElapsed = (uint32_t)(micros() - touchReadStart);
    ++sTouchReadCount;
    sTouchReadTotalUs += touchReadElapsed;
    if (touchReadElapsed > sTouchReadMaxUs) sTouchReadMaxUs = touchReadElapsed;
    if (sTouch.isTouched && sTouch.touches > 0) ++sTouchPressedReads;
    if (sTouchDiagMode == 2) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // FIX8B: Restore immediate GT911 press/release forwarding.
    // The 12/30 ms filter in FIX8A suppressed short touches because
    // LVGL polls the input device at discrete intervals.
    const bool pressed = (sTouch.isTouched && sTouch.touches > 0);

    // V0.13C FIX2: a modal CLOSE must consume its whole physical gesture.
    // While quarantined, LVGL sees RELEASED even if GT911 produces a short
    // second press. Re-enable only after a continuous released interval.
    if (sTouchSuppressUntilRelease) {
        const uint32_t nowMs = millis();
        data->state = LV_INDEV_STATE_REL;
        if (pressed) {
            sTouchSuppressReleaseSinceMs = 0;
        } else {
            if (sTouchSuppressReleaseSinceMs == 0)
                sTouchSuppressReleaseSinceMs = nowMs;
            else if ((uint32_t)(nowMs - sTouchSuppressReleaseSinceMs) >=
                     sTouchSuppressSettleMs) {
                sTouchSuppressUntilRelease = false;
                sTouchSuppressReleaseSinceMs = 0;
            }
        }
    } else if (pressed) {
        data->point.x = constrain(sTouch.points[0].x, 0, LCD_WIDTH - 1);
        data->point.y = constrain(sTouch.points[0].y, 0, LCD_HEIGHT - 1);
        sTouchX = data->point.x;
        sTouchY = data->point.y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    static uint32_t lastTouchPrint = 0;
    if (pressed && (uint32_t)(millis() - lastTouchPrint) > 300) {
        lastTouchPrint = millis();
        
    }
    if (pressed != sTouchPressed) {
        sTouchPressed = pressed;
        sTouchLastChangeMs = millis();
    }
}

bool Board::begin()
{
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, LOW);

    // -------------------------------------------------------------------------
    // 1. SD first - exact proven MiniAmused pattern
    // -------------------------------------------------------------------------
    
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    delay(10);

    
    sSdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
    delay(20);

    
    sSDReady = SD.begin(SD_CS, sSdSPI, SD_SPEED);

    if (sSDReady) {
        const uint8_t cardType = SD.cardType();
        const uint64_t cardSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
        
    } else {
        
    }

    // -------------------------------------------------------------------------
    // 2. QSPI / NV3041A after SD
    // -------------------------------------------------------------------------
    
    sBus = new Arduino_ESP32QSPI(GFX_CS, GFX_SCK, GFX_D0, GFX_D1, GFX_D2, GFX_D3);
    if (!sBus) {
        
        return false;
    }
    

    
    sPanel = new Arduino_NV3041A(sBus, GFX_NOT_DEFINED, 0, true);
    sGfx = sPanel;
    if (!sPanel) {
        
        return false;
    }
    

    // Controlled LCD reset BEFORE NV3041A initialization.
    // External 220-ohm series resistor from GPIO5 to LCD RESET required.
    // Open-drain never actively drives the RC reset node HIGH.
#if RTAL_LCD_HW_RESET_ENABLE
    
    digitalWrite(RTAL_LCD_RESET_GPIO, LOW);
    pinMode(RTAL_LCD_RESET_GPIO, OUTPUT_OPEN_DRAIN);
    delay(RTAL_LCD_RESET_LOW_MS);
    digitalWrite(RTAL_LCD_RESET_GPIO, HIGH); // Open-drain HIGH = release
    delay(RTAL_LCD_RESET_RELEASE_MS);
    
#else
    
#endif

    
    if (!sGfx->begin(GFX_SPEED)) {
        
        return false;
    }
    
    
    // V0.3A: no diagnostic RGB bars, no 1.5 second boot delay.
    sGfx->fillScreen(RGB565_BLACK);

    // Keep proven MiniAmused panel initialization, omit its branded splash.
    digitalWrite(GFX_BL, HIGH);

    // -------------------------------------------------------------------------
    // 3. LVGL 8.4.0
    // -------------------------------------------------------------------------
    
    lv_init();

    
    sDrawBuf = static_cast<lv_color_t *>(
        heap_caps_malloc(LVGL_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
    );
    if (!sDrawBuf) {
        sDrawBuf = static_cast<lv_color_t *>(malloc(LVGL_BUF_BYTES));
    }
    if (!sDrawBuf) {
        
        return false;
    }
    
    

    lv_disp_draw_buf_init(&sDrawBufDesc, sDrawBuf, nullptr, LVGL_BUF_PIXELS);

    
    lv_disp_drv_init(&sDispDrv);
    sDispDrv.hor_res = LCD_WIDTH;
    sDispDrv.ver_res = LCD_HEIGHT;
    sDispDrv.flush_cb = flushCB;
    sDispDrv.draw_buf = &sDrawBufDesc;
    sDisplay = lv_disp_drv_register(&sDispDrv);
    if (!sDisplay) {
        
        return false;
    }
    // STAGE4: LVGL input remains responsive, but full display refresh is
    // limited to 25 Hz to reduce shared flash/cache/QSPI pressure on Core1.
    // LVGL 8.4 display owns its internal refresh timer.
    if (sDisplay->refr_timer) lv_timer_set_period(sDisplay->refr_timer, 40);
    

    // -------------------------------------------------------------------------
    // 4. GT911 + LVGL 8.4 input driver
    // -------------------------------------------------------------------------
    
    sTouch.begin(GT911_ADDR1);
    sTouch.setRotation(ROTATION_INVERTED);

    
    lv_indev_drv_init(&sIndevDrv);
    sIndevDrv.type = LV_INDEV_TYPE_POINTER;
    sIndevDrv.read_cb = touchCB;
    sIndev = lv_indev_drv_register(&sIndevDrv);
    if (!sIndev) {
        
        return false;
    }
    

    digitalWrite(GFX_BL, HIGH);
    
    return true;
}


bool Board::showSplashRGB565(const uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (!sGfx || !pixels) return false;
    if (width != LCD_WIDTH || height != LCD_HEIGHT) return false;
    // ESP32-S3 flash is memory mapped, so Arduino_GFX can stream the PROGMEM
    // RGB565 words directly without allocating a 261 kB RAM copy.
    sGfx->draw16bitRGBBitmap(0, 0, const_cast<uint16_t *>(pixels), width, height);
    return true;
}


void Board::serviceLVGL()
{
    // LVGL 8.x has no lv_tick_set_cb() used by our former LVGL 9 port.
    // Feed the LVGL tick from Arduino millis() on every service call, even if
    // the expensive timer handler itself is throttled to 2 ms.
    static uint32_t lastTickMs = 0;
    static uint32_t lastHandlerMs = 0;
    const uint32_t now = millis();

    if(lastTickMs == 0) lastTickMs = now;
    const uint32_t elapsed = (uint32_t)(now - lastTickMs);
    if(elapsed) {
        lv_tick_inc(elapsed);
        lastTickMs = now;
    }

    if((uint32_t)(now - lastHandlerMs) < sLvglIntervalMs7Z)
        return;
    lastHandlerMs = now;

    if(sDisplayTestMode != 2 && !sLvglHandlerPaused8A) {
        const uint32_t startUs = micros();
        lv_timer_handler();
        const uint32_t durationUs = (uint32_t)(micros() - startUs);
        ++sLvglHandlerCalls7Z;
        if(durationUs > sLvglHandlerMaxUs7Z) sLvglHandlerMaxUs7Z = durationUs;
        if(durationUs > 3000) ++sLvglHandlerOver3ms7Z;
    }
}

bool Board::retrySD(uint32_t speedHz) {
    // Only called on the Core0 GUI/setup thread while no SD File is open.
    // Do not touch QSPI, LVGL, GT911, I2S or MIDI.
    
    SD.end();
    sSDReady=false;
    sSdSPI.end();
    pinMode(SD_CS,OUTPUT);
    digitalWrite(SD_CS,HIGH);
    delay(80);
    sSdSPI.begin(SD_SCK,SD_MISO,SD_MOSI,-1);
    delay(40);
    sSDReady=SD.begin(SD_CS,sSdSPI,speedHz);
    if(sSDReady && SD.cardType()!=CARD_NONE) {
        File dir=SD.open("/RTAL_MIOSAURUS/PRESETS",FILE_READ);
        const bool directoryOK=(bool)dir && dir.isDirectory();
        if(dir)dir.close();
        
        return true; // Mount and preset data are deliberately reported separately.
    }
    sSDReady=false;
    
    return false;
}
bool Board::sdReady() { return sSDReady; }
bool Board::touchPressed() { return sTouchPressed; }
bool Board::touchPosition(int16_t &x,int16_t &y) {
    if(!sTouchPressed)return false;
    x=sTouchX;y=sTouchY;
    return true;
}
uint32_t Board::touchLastChangeMs() { return sTouchLastChangeMs; }

void Board::suppressTouchUntilReleased(uint32_t settleMs) {
    if (settleMs < 30) settleMs = 30;
    if (settleMs > 300) settleMs = 300;
    sTouchSuppressSettleMs = settleMs;
    sTouchSuppressReleaseSinceMs = 0;
    sTouchSuppressUntilRelease = true;
    // Drop LVGL's current pointer ownership immediately. The physical read
    // quarantine above then absorbs any tail/ghost samples from this gesture.
    if (sIndev) lv_indev_reset(sIndev, nullptr);
}

uint32_t Board::guiFrameCount() { return sGuiFrames; }
uint32_t Board::guiFlushCount() { return sGuiFlushes; }
uint64_t Board::guiPixelCount() { return sGuiPixels; }

// FIX7U: Called only from the Core0 GUI/serial owner.
void Board::setTouchDiagnosticMode(uint8_t mode) {
    if (mode > 2) return;
    sTouchDiagMode = mode;
    sTouchReadCount = sTouchReadTotalUs = sTouchReadMaxUs = 0;
    sTouchPressedReads = sTouchCBCount = 0;
    sTouchPressed = false;
    
}
uint8_t Board::touchDiagnosticMode() { return sTouchDiagMode; }
void Board::printTouchDiagnostics() {
    const uint32_t n = sTouchReadCount;
    
}

void Board::setDisplayTestMode(uint8_t mode) { if(mode <= 2) { sDisplayTestMode=mode; if(mode==0 && sDisplay) lv_obj_invalidate(lv_scr_act()); } }
uint8_t Board::displayTestMode() { return sDisplayTestMode; }
void Board::printDisplayTestStats() { }

// FIX7Z runtime diagnostics; invoked only from the GUI owner task.
void Board::setLvglInterval7Z(uint32_t ms) { if(ms>=5 && ms<=200) sLvglIntervalMs7Z=ms; }
void Board::printLvglIntervalStats7Z() {
    
}

// FIX8A: Diagnostic-only controls, owned by the Core0 GUI task.
void Board::setLvglHandlerPaused8A(bool paused) {
    if(sLvglHandlerPaused8A && !paused && sDisplay) lv_obj_invalidate(lv_scr_act());
    sLvglHandlerPaused8A = paused;
}
bool Board::lvglHandlerPaused8A() { return sLvglHandlerPaused8A; }
void Board::printMemoryStats8A() {
    const uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const uint32_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const uint32_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
}
