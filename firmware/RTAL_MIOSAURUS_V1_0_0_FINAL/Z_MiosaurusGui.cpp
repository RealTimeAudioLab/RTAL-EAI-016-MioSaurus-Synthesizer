#include "Z_MiosaurusGui.h"
#include "Z_MiosaurusState.h"
#include "Z_MiosaurusPerformance.h"
#include "Z_MiosaurusPreset.h"
#include "Z_MiosaurusMidi.h"
#include "Board.h"
#include <math.h>

// -----------------------------------------------------------------------------
// MIOSAURUS state bridge
// Large horizontal sliders, page buttons, preset header and inline help.
// The GUI values are normalized 0..1 for this isolated hardware test.
// ----------------------------------------------------------------------------

static const lv_color_t COL_BG     = lv_color_hex(0x0D0A08);
static const lv_color_t COL_PANEL  = lv_color_hex(0x25170E);
static const lv_color_t COL_ACC    = lv_color_hex(0xE99A32);
static const lv_color_t COL_BORDER = lv_color_hex(0x8D5927);
static const lv_color_t COL_TEXT   = lv_color_hex(0xF4E5CF);
static const lv_color_t COL_MUTED  = lv_color_hex(0xB89C7A);
static const lv_color_t COL_DARK   = lv_color_hex(0x21130B);

struct ParamDef { MioParamId id; const char *title; };

static const char *PAGE_NAMES[7] = {
  "MAIN", "OSC", "FILTER", "F-ENV", "A-ENV", "MOD", "FX"
};

static const ParamDef PAGE_DEFS[6][6] = {
  { // MAIN
    {P_OSC1_LEVEL,"OSC1 LEVEL"}, {P_OSC2_LEVEL,"OSC2 LEVEL"}, {P_SUB_LEVEL,"SUB LEVEL"},
    {P_FILTER_CUTOFF,"CUTOFF"}, {P_FILTER_RESO,"RESO"}, {P_MASTER_VOL,"MASTER"}
  },
  { // OSC
    {P_OSC1_WAVE,"OSC1 MORPH"}, {P_OSC1_PW,"OSC1 PW"}, {P_OSC1_PWM_AMT,"OSC1 PWM"},
    {P_OSC2_WAVE,"OSC2 MORPH"}, {P_OSC2_PW,"OSC2 PW"}, {P_OSC2_PWM_AMT,"OSC2 PWM"}
  },
  { // FILTER
    {P_FILTER_CUTOFF,"CUTOFF"}, {P_FILTER_RESO,"RESO"}, {P_FILTER_DRIVE,"DRIVE"},
    {P_FILTER_ENV_AMT,"ENV AMT"}, {P_FILTER_KEYTRACK,"KEYTRK"}, {P_FILTER_VEL_AMT,"VEL->FLT"}
  },
  { // FILTER ENV
    {P_FILTER_ATTACK,"ATTACK"}, {P_FILTER_DECAY,"DECAY"}, {P_FILTER_SUSTAIN,"SUSTAIN"},
    {P_FILTER_RELEASE,"RELEASE"}, {P_FILTER_ENV_AMT,"ENV AMT"}, {P_FILTER_VEL_AMT,"VEL AMT"}
  },
  { // AMP ENV
    {P_AMP_ATTACK,"ATTACK"}, {P_AMP_DECAY,"DECAY"}, {P_AMP_SUSTAIN,"SUSTAIN"},
    {P_AMP_RELEASE,"RELEASE"}, {P_MASTER_VOL,"MASTER"}, {P_GLIDE,"GLIDE"}
  },
  { // MOD
    {P_LFO_RATE,"LFO RATE"}, {P_LFO_FILTER_AMT,"LFO->FLT"}, {P_LFO_SYNC,"LFO SYNC"},
    {P_LFO_DIV,"LFO DIV"}, {P_OSC2_DETUNE,"OSC2 DET"}, {P_PB_RANGE,"PB RANGE"}
  }
};

static float gValue[PARAM_COUNT];
static lv_obj_t *gRoot = nullptr;
static lv_obj_t *gPresetLabel = nullptr;
static lv_obj_t *gModeLabel = nullptr;
static lv_obj_t *gLegatoButton = nullptr;
static lv_obj_t *gPriorityButton = nullptr;
static lv_obj_t *gMidiButton = nullptr;
static lv_obj_t *gLearnOverlay = nullptr;
static lv_obj_t *gLearnBankButton = nullptr;
static lv_obj_t *gMidiChannelButton = nullptr;
static lv_obj_t *gLearnTargetLabel = nullptr;
static lv_obj_t *gLearnCcLabel = nullptr;
static lv_obj_t *gLearnStatusLabel = nullptr;
static uint8_t gLearnBank = 0;
static uint8_t gLearnTarget = 0;
static uint32_t gLastLearnRevision = 0;
static lv_obj_t *gPresetButton = nullptr;
static lv_obj_t *gPresetPanel = nullptr;
static lv_obj_t *gPresetPanelTitle = nullptr;
static lv_obj_t *gPresetPanelStatus = nullptr;
static lv_obj_t *gPresetConfirmLabel = nullptr;
static lv_obj_t *gPresetConfirmYes = nullptr;
static lv_obj_t *gPresetConfirmNo = nullptr;
static bool gPresetPanelOpen = false;
static uint32_t gLastPresetRevision = 0;

enum GuiPresetConfirm : uint8_t {
  GUI_PRESET_CONFIRM_NONE = 0,
  GUI_PRESET_CONFIRM_LOAD,
  GUI_PRESET_CONFIRM_SAVEAS
};
static GuiPresetConfirm gPresetConfirm = GUI_PRESET_CONFIRM_NONE;
static lv_obj_t *gPresetNameField = nullptr;
static lv_obj_t *gNameOverlay = nullptr;
static lv_obj_t *gNameTextArea = nullptr;
static lv_obj_t *gNameKeyboard = nullptr;
static char gPresetEditName[MIO_PRESET_NAME_MAX + 1] = "INIT PATCH";
static bool gPresetNameDirty = false;
static uint8_t gPresetNameSlot = 255;
static lv_obj_t *gPageArea = nullptr;
static lv_obj_t *gPageButtons[7] = {};
static lv_obj_t *gSliders[6] = {};
static lv_obj_t *gValueLabels[6] = {};
static MioParamId gControlIds[6];
static lv_obj_t *gInfo = nullptr;
static lv_obj_t *gFxTabButtons[3] = {};
static int gPage = 0;
static int gFxTab = 0; // 0 flanger, 1 delay, 2 light reverb
static uint32_t gLastPagePressMs = 0;
static uint32_t gLastFxTabPressMs = 0;
static uint32_t gLastSliderUpdateMs[6] = {};
static bool gSliderPending[6] = {};
static int16_t gSliderPendingValue[6] = {};
static constexpr uint32_t PAGE_DEBOUNCE_MS = 90;
static constexpr uint32_t SLIDER_INTERVAL_MS = 18;

struct SafeButtonBinding {
  lv_obj_t *obj;
  lv_event_cb_t cb;
  intptr_t id;
  uint32_t lastFireMs;
};

static SafeButtonBinding gSafeButtons[64] = {};
static constexpr uint32_t BUTTON_REFIRE_GUARD_MS = 220;
static uint32_t gLastMidiChannelCycleMs = 0;
static constexpr uint32_t MIDI_CHANNEL_REFIRE_GUARD_MS = 500;
static uint32_t gLastPresetArrowMs = 0;
static constexpr uint32_t PRESET_ARROW_SINGLE_STEP_MS = 550;
static uint32_t gLastPresetPanelArrowMs = 0;
static constexpr uint32_t PRESET_PANEL_ARROW_GUARD_MS = 550;

static bool gNameOpenPending = false;
static uint32_t gNameOpenAfterMs = 0;
static uint32_t gLastNameFieldReleaseMs = 0;
static constexpr uint32_t NAME_FIELD_GUARD_MS = 450;

static char gKeyboardAcceptedText[MIO_PRESET_NAME_MAX + 1] = "";
static uint32_t gLastKeyboardAcceptMs = 0;
static constexpr uint32_t KEYBOARD_DUP_GUARD_MS = 170;

static void safeButtonDispatch(lv_event_t *e) {
  SafeButtonBinding *b=(SafeButtonBinding*)lv_event_get_user_data(e);
  if(!b || !b->cb || !b->obj) return;

  const uint32_t now=millis();
  if(b->lastFireMs && (uint32_t)(now-b->lastFireMs)<BUTTON_REFIRE_GUARD_MS) return;
  b->lastFireMs=now;

  // Re-dispatch the same LVGL event with the original integer user-data.
  lv_event_t forwarded=*e;
  forwarded.user_data=(void*)b->id;
  b->cb(&forwarded);
}

// V0.14B FIX1: buildPage() destroys and recreates FX tab buttons.  The old
// fixed append-only binding pool eventually filled up, leaving newly-created
// buttons visually pressable but without a RELEASE callback.  Recycle a slot
// when LVGL deletes its object so FLANGER/DELAY/REVERB remain selectable forever.
static void safeButtonDeleted(lv_event_t *e) {
  SafeButtonBinding *b=(SafeButtonBinding*)lv_event_get_user_data(e);
  if(!b) return;
  b->obj=nullptr;
  b->cb=nullptr;
  b->id=0;
  b->lastFireMs=0;
}

static SafeButtonBinding *allocSafeButtonBinding(lv_obj_t *obj,
                                                 lv_event_cb_t cb,
                                                 intptr_t id) {
  for(size_t i=0;i<(sizeof(gSafeButtons)/sizeof(gSafeButtons[0]));++i) {
    if(gSafeButtons[i].obj==nullptr) {
      SafeButtonBinding &b=gSafeButtons[i];
      b.obj=obj;
      b.cb=cb;
      b.id=id;
      b.lastFireMs=0;
      return &b;
    }
  }
  return nullptr;
}

static lv_obj_t *makeButton(lv_obj_t *parent, const char *label,
                            int x, int y, int w, int h,
                            lv_event_cb_t cb, intptr_t id) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_pos(b,x,y); lv_obj_set_size(b,w,h);
  lv_obj_set_style_bg_color(b,COL_PANEL,0);
  lv_obj_set_style_radius(b,7,0);
  lv_obj_set_style_border_width(b,1,0);
  lv_obj_set_style_border_color(b,COL_BORDER,0);
  lv_obj_set_style_pad_all(b,0,0);
  lv_obj_clear_flag(b,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b,LV_OBJ_FLAG_PRESS_LOCK);

  if(cb) {
    SafeButtonBinding *bind=allocSafeButtonBinding(b,cb,id);
    if(bind) {
      lv_obj_add_event_cb(b,safeButtonDispatch,LV_EVENT_RELEASED,bind);
      lv_obj_add_event_cb(b,safeButtonDeleted,LV_EVENT_DELETE,bind);
    } else {
      // Fail functional rather than fail silent if the pool is ever exhausted.
      lv_obj_add_event_cb(b,cb,LV_EVENT_RELEASED,(void*)id);
    }
  }

  lv_obj_t *l=lv_label_create(b);
  lv_label_set_text(l,label);
  lv_obj_set_style_text_color(l,COL_TEXT,0);
  lv_obj_center(l);
  return b;
}

static void formatValue(MioParamId id, int v, char *buf, size_t n) {
  v = constrain(v,0,127);
  const float norm = v / 127.0f;
  const float actual = miosaurusStateActualFromNormalized(id,norm);

  switch(id) {
    case P_OSC1_WAVE:
    case P_OSC2_WAVE:
      if(v<=0) snprintf(buf,n,"SAW");
      else if(v>=127) snprintf(buf,n,"SQR");
      else snprintf(buf,n,"M%03d",v);
      break;

    case P_LFO_SYNC:
    case P_DLY_SYNC:
      snprintf(buf,n,"%s",v>=64?"SYNC":"FREE");
      break;

    case P_FLG_POLARITY:
      snprintf(buf,n,"%s",v>=64?"NEG":"POS");
      break;

    case P_FILTER_ENV_AMT:
    case P_LFO_FILTER_AMT:
      // Bipolar modulation depth: negative=inverted, positive=normal.
      snprintf(buf,n,"%+.0f%%",actual*100.0f);
      break;

    case P_OSC2_DETUNE:
      snprintf(buf,n,"%+.1fst",actual);
      break;

    case P_FILTER_ATTACK:
    case P_FILTER_DECAY:
    case P_FILTER_RELEASE:
    case P_AMP_ATTACK:
    case P_AMP_DECAY:
    case P_AMP_RELEASE:
      if(actual < 1.0f) snprintf(buf,n,"%.0fms",actual*1000.0f);
      else snprintf(buf,n,"%.2fs",actual);
      break;

    case P_FILTER_DRIVE:
      snprintf(buf,n,"%.2fx",actual);
      break;

    case P_PB_RANGE:
      snprintf(buf,n,"%.1fst",actual);
      break;

    case P_GLIDE: {
      // V1.0.0 FINAL: display the musical glide duration instead of 0..100 %.
      // Keep this mapping identical to Z_MiosaurusAudio.cpp updateParams().
      // The stored state, presets and MIDI CC78 remain normalized 0..1.
      if (v <= 0) {
        snprintf(buf,n,"OFF");
      } else {
        const float g=constrain(norm,0.0f,1.0f);
        const float sec=0.003f + powf(g,2.2f)*2.997f;
        if(sec < 1.0f) snprintf(buf,n,"%.0fms",sec*1000.0f);
        else snprintf(buf,n,"%.2fs",sec);
      }
      break;
    }

    case P_LFO_DIV:
    case P_DLY_DIV: {
      // Both sync engines use the same 11 musical beat divisions.
      // V0.14B: LFO DIV no longer shows a misleading 0..100 %.
      static const char *labels[11] = {
        "1/16T","1/16","1/16D","1/8T","1/8","1/8D",
        "1/4T","1/4","1/4D","1/2","1/1"
      };
      int idx=(int)(norm*10.999f);
      idx=constrain(idx,0,10);
      snprintf(buf,n,"%s",labels[idx]);
      break;
    }

    case P_DLY_TIME: {
      // Must match updateFxParams(): 0..1 -> 0.25..2.00 seconds.
      const float sec=0.25f + constrain(actual,0.0f,1.0f)*1.75f;
      if(sec < 1.0f) snprintf(buf,n,"%.0fms",sec*1000.0f);
      else snprintf(buf,n,"%.2fs",sec);
      break;
    }

    case P_DLY_DAMP: {
      // Must match PingPongDelay::process():
      // dampCoeff = 0.05 + (1-damp)*0.45.
      // Convert its one-pole coefficient to the equivalent -3 dB-ish
      // exponential cutoff frequency at the fixed 32 kHz audio rate.
      const float d=constrain(actual,0.0f,1.0f);
      const float a=0.05f + (1.0f-d)*0.45f;
      const float fc=-(32000.0f/(2.0f*PI))*logf(max(0.000001f,1.0f-a));
      if(fc >= 1000.0f) snprintf(buf,n,"%.1fkHz",fc/1000.0f);
      else snprintf(buf,n,"%.0fHz",fc);
      break;
    }

    default:
      snprintf(buf,n,"%d%%",(int)lroundf(norm*100.0f));
      break;
  }
}

static void showLastParameterChange(MioParamId id, int sliderValue) {
  if(!gInfo || id>=PARAM_COUNT) return;

  char display[28];
  formatValue(id,sliderValue,display,sizeof(display));

  const MioParamStateDef &d=miosaurusStateDef(id);
  const float actual=miosaurusStateActualFromNormalized(
      id, constrain(sliderValue,0,127)/127.0f);

  char b[96];
  if(d.discrete) {
    snprintf(b,sizeof(b),"LAST: %s = %s",d.name,display);
  } else if(id==P_FILTER_ATTACK || id==P_FILTER_DECAY || id==P_FILTER_RELEASE ||
            id==P_AMP_ATTACK || id==P_AMP_DECAY || id==P_AMP_RELEASE) {
    snprintf(b,sizeof(b),"LAST: %s = %.3f s",d.name,actual);
  } else if(id==P_OSC2_DETUNE) {
    snprintf(b,sizeof(b),"LAST: %s = %+.2f st",d.name,actual);
  } else if(id==P_PB_RANGE) {
    snprintf(b,sizeof(b),"LAST: %s = %.1f st",d.name,actual);
  } else if(id==P_FILTER_DRIVE) {
    snprintf(b,sizeof(b),"LAST: %s = %.2fx",d.name,actual);
  } else {
    snprintf(b,sizeof(b),"LAST: %s = %s",d.name,display);
  }
  lv_label_set_text(gInfo,b);
}

static void refreshSliderValue(int slot) {
  if(slot<0 || slot>=6 || !gValueLabels[slot]) return;
  char b[24];
  formatValue(gControlIds[slot], lv_slider_get_value(gSliders[slot]), b, sizeof(b));
  lv_label_set_text(gValueLabels[slot],b);
}

static void flushPending() {
  const uint32_t now=millis();
  for(int i=0;i<6;i++) {
    if(!gSliderPending[i]) continue;
    if(gLastSliderUpdateMs[i] && (uint32_t)(now-gLastSliderUpdateMs[i])<SLIDER_INTERVAL_MS) continue;
    const int v=gSliderPendingValue[i];
    gSliderPending[i]=false;
    gLastSliderUpdateMs[i]=now;
    MioParamId id=gControlIds[i];
    gValue[id]=constrain(v,0,127)/127.0f;
    refreshSliderValue(i);
    miosaurusGuiOnParamChanged(id,gValue[id]);
  }
}


static float delayBeatsForDivNorm(float norm) {
  static const float beats[11] = {
    1.0f/6.0f, 0.25f, 0.375f, 1.0f/3.0f, 0.5f, 0.75f,
    2.0f/3.0f, 1.0f, 1.5f, 2.0f, 4.0f
  };
  int idx=(int)(constrain(norm,0.0f,1.0f)*10.999f);
  idx=constrain(idx,0,10);
  return beats[idx];
}

static void syncDelayTimeFromDivision() {
  if(gValue[P_DLY_SYNC] < 0.5f) return;

  const float bpm=max(20.0f,miosaurusPerformanceBpm());
  const float beats=delayBeatsForDivNorm(gValue[P_DLY_DIV]);
  float sec=(60.0f/bpm)*beats;

  // TIME parameter range in current DSP: 250 ms .. 2.00 s.
  sec=constrain(sec,0.25f,2.0f);
  const float timeNorm=constrain((sec-0.25f)/1.75f,0.0f,1.0f);
  gValue[P_DLY_TIME]=timeNorm;
  miosaurusGuiOnParamChanged(P_DLY_TIME,timeNorm);

  // If TIME is visible on the current FX/DELAY page, move its slider too.
  for(int i=0;i<6;i++) {
    if(gSliders[i] && gControlIds[i]==P_DLY_TIME) {
      const int sv=(int)lroundf(timeNorm*127.0f);
      lv_slider_set_value(gSliders[i],sv,LV_ANIM_OFF);
      refreshSliderValue(i);
      break;
    }
  }
}

static void onSlider(lv_event_t *e) {
  lv_event_code_t code=lv_event_get_code(e);
  if(code!=LV_EVENT_VALUE_CHANGED && code!=LV_EVENT_RELEASED && code!=LV_EVENT_PRESS_LOST) return;
  int slot=(int)(intptr_t)lv_event_get_user_data(e);
  if(slot<0 || slot>=6) return;
  int v=lv_slider_get_value(lv_event_get_target(e));
  gSliderPending[slot]=true;
  gSliderPendingValue[slot]=v;
  refreshSliderValue(slot);
  showLastParameterChange(gControlIds[slot],v);

  const MioParamId changedId=gControlIds[slot];
  if(changedId==P_DLY_DIV ||
     (changedId==P_DLY_SYNC && v>=64)) {
    // Use the new slider value immediately, before queue flush.
    gValue[changedId]=constrain(v,0,127)/127.0f;
    syncDelayTimeFromDivision();
  }

  if(code!=LV_EVENT_VALUE_CHANGED) {
    gLastSliderUpdateMs[slot]=0;
    flushPending();
  }
}

static void addSlider(int slot, MioParamId id, const char *label, int x, int y, int width) {
  if(slot<0 || slot>=6) return;
  gControlIds[slot]=id;

  // FIX8: every value field must remain on one line, including engineering
  // units such as 850ms, 1.25s, +12.0st and 1.8kHz.
  int labelW=70;
  int sliderX=74;
  int sliderW=width-140;
  int valueX=width-60;
  int valueW=58;

  // MOD needs breathing room on both sides.
  if(gPage==5) {
    labelW=62;
    sliderX=66;
    sliderW=96;
    valueX=168;
    valueW=61;
  }

  // FX already used wide engineering-value fields; keep that successful layout.
  if(gPage==6) {
    labelW=54;
    sliderX=60;
    sliderW=105;
    valueX=168;
    valueW=61;
  }

  lv_obj_t *name=lv_label_create(gPageArea);
  lv_label_set_text(name,label);
  lv_obj_set_pos(name,x+3,y+2);
  lv_obj_set_style_text_color(name,COL_TEXT,0);
  lv_obj_set_width(name,labelW);
  lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);

  lv_obj_t *sl=lv_slider_create(gPageArea);
  gSliders[slot]=sl;
  lv_obj_set_pos(sl,x+sliderX,y+7);
  lv_obj_set_size(sl,sliderW,14);
  lv_obj_set_ext_click_area(sl,8);
  lv_obj_add_flag(sl,LV_OBJ_FLAG_PRESS_LOCK);
  lv_slider_set_range(sl,0,127);
  lv_slider_set_value(sl,(int)lroundf(constrain(gValue[id],0.0f,1.0f)*127.0f),LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl,COL_DARK,LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl,COL_ACC,LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl,COL_TEXT,LV_PART_KNOB);
  lv_obj_add_event_cb(sl,onSlider,LV_EVENT_VALUE_CHANGED,(void*)(intptr_t)slot);
  lv_obj_add_event_cb(sl,onSlider,LV_EVENT_RELEASED,(void*)(intptr_t)slot);
  lv_obj_add_event_cb(sl,onSlider,LV_EVENT_PRESS_LOST,(void*)(intptr_t)slot);

  lv_obj_t *value=lv_label_create(gPageArea);
  gValueLabels[slot]=value;
  lv_obj_set_pos(value,x+valueX,y+2);
  lv_obj_set_size(value,valueW,20);
  lv_label_set_long_mode(value,LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(value,LV_TEXT_ALIGN_RIGHT,0);
  lv_obj_set_style_text_color(value,COL_ACC,0);
  refreshSliderValue(slot);
}

static void buildPage();
static void refreshPresetHeader();
static void refreshPresetPanel();

static void onPage(lv_event_t *e) {
  uint32_t now=millis();
  if((uint32_t)(now-gLastPagePressMs)<PAGE_DEBOUNCE_MS) return;
  gLastPagePressMs=now;
  gPage=(int)(intptr_t)lv_event_get_user_data(e);
  buildPage();
  refreshPresetHeader();
  refreshPresetPanel();
}

static void onFxTab(lv_event_t *e) {
  const uint32_t now=millis();
  // Separate from page-navigation debounce: entering FX must not suppress an
  // immediately following DELAY/REVERB tab selection.
  if(gLastFxTabPressMs && (uint32_t)(now-gLastFxTabPressMs)<PAGE_DEBOUNCE_MS) return;
  gLastFxTabPressMs=now;
  const int nextTab=(int)(intptr_t)lv_event_get_user_data(e);
  if(nextTab<0 || nextTab>2 || nextTab==gFxTab) return;
  gFxTab=nextTab;
  buildPage();
}

static void updatePageButtons() {
  for(int i=0;i<7;i++) {
    bool active=(i==gPage);
    lv_obj_set_style_bg_color(gPageButtons[i],active?COL_ACC:COL_PANEL,0);
    lv_obj_t *l=lv_obj_get_child(gPageButtons[i],0);
    if(l) lv_obj_set_style_text_color(l,active?lv_color_hex(0x241208):COL_TEXT,0);
  }
}

static void buildPage() {
  flushPending();
  if(!gPageArea) return;
  lv_obj_clean(gPageArea);
  for(int i=0;i<6;i++) {
    gSliders[i]=nullptr; gValueLabels[i]=nullptr; gLastSliderUpdateMs[i]=0; gSliderPending[i]=false;
  }
  updatePageButtons();

  if(gPage<6) {
    for(int i=0;i<6;i++) {
      int col=i/3, row=i%3;
      const ParamDef &d=PAGE_DEFS[gPage][i];
      addSlider(i,d.id,d.title,col*234,7+row*40,232);
    }
  } else {
    gFxTabButtons[0]=makeButton(gPageArea,"FLANGER",4,2,105,25,onFxTab,0);
    gFxTabButtons[1]=makeButton(gPageArea,"DELAY",114,2,105,25,onFxTab,1);
    gFxTabButtons[2]=makeButton(gPageArea,"REVERB",224,2,105,25,onFxTab,2);
    for(int i=0;i<3;i++) {
      bool a=(i==gFxTab);
      lv_obj_set_style_bg_color(gFxTabButtons[i],a?COL_ACC:COL_PANEL,0);
      lv_obj_t *l=lv_obj_get_child(gFxTabButtons[i],0);
      if(l) lv_obj_set_style_text_color(l,a?lv_color_hex(0x241208):COL_TEXT,0);
    }

    static const ParamDef flg[6]={
      {P_FLG_WET,"VOL"},{P_FLG_RATE,"RATE"},{P_FLG_DEPTH,"DEPTH"},
      {P_FLG_FB,"FDBK"},{P_FLG_MANUAL,"MANU"},{P_FLG_WIDTH,"WIDTH"}
    };
    static const ParamDef dly[6]={
      {P_DLY_WET,"VOL"},{P_DLY_FB,"FDBK"},{P_DLY_TIME,"TIME"},
      {P_DLY_DAMP,"DAMP"},{P_DLY_DIV,"DIV"},{P_DLY_SYNC,"SYNC"}
    };
    static const ParamDef rev[4]={
      {P_REV_VOL,"VOL"},{P_REV_SIZE,"SIZE"},
      {P_REV_DECAY,"DECAY"},{P_REV_DAMP,"DAMP"}
    };
    const ParamDef *defs=(gFxTab==0)?flg:((gFxTab==1)?dly:rev);
    const int count=(gFxTab==2)?4:6;
    for(int i=0;i<count;i++) {
      int col=i/3,row=i%3;
      addSlider(i,defs[i].id,defs[i].title,col*234,32+row*32,232);
    }
    return;
  }
}

static bool presetArrowMayFire() {
  const uint32_t now=millis();
  if(gLastPresetArrowMs &&
     (uint32_t)(now-gLastPresetArrowMs)<PRESET_ARROW_SINGLE_STEP_MS) {
    return false;
  }
  gLastPresetArrowMs=now;
  return true;
}

static void onPresetPrev(lv_event_t *e) {
  (void)e;
  if(!presetArrowMayFire()) return;
  miosaurusPresetBrowsePrev();
  miosaurusPresetLoadBrowse();
}
static void onPresetNext(lv_event_t *e) {
  (void)e;
  if(!presetArrowMayFire()) return;
  miosaurusPresetBrowseNext();
  miosaurusPresetLoadBrowse();
}


static void refreshPresetHeader() {
  if(!gPresetLabel) return;

  const uint8_t loaded=miosaurusPresetLoadedSlot();
  const uint8_t browse=miosaurusPresetBrowseSlot();
  const bool edited=miosaurusPresetEdited();

  char name[MIO_PRESET_NAME_MAX+1];
  bool exists=miosaurusPresetGetSlotName(browse,name,sizeof(name));

  char b[96];
  if(browse==loaded) {
    snprintf(b,sizeof(b),"P%03u%s  %s",
             (unsigned)browse,edited?"*":"",
             exists?name:miosaurusPresetLoadedName());
  } else {
    snprintf(b,sizeof(b),"P%03u  %s", (unsigned)browse,
             exists?name:"EMPTY");
  }
  lv_label_set_text(gPresetLabel,b);
}

static void closePresetConfirm() {
  gPresetConfirm=GUI_PRESET_CONFIRM_NONE;
  if(gPresetConfirmLabel) lv_obj_add_flag(gPresetConfirmLabel,LV_OBJ_FLAG_HIDDEN);
  if(gPresetConfirmYes) lv_obj_add_flag(gPresetConfirmYes,LV_OBJ_FLAG_HIDDEN);
  if(gPresetConfirmNo) lv_obj_add_flag(gPresetConfirmNo,LV_OBJ_FLAG_HIDDEN);
}

static void refreshPresetPanel() {
  if(!gPresetPanel || !gPresetPanelOpen) return;

  const uint8_t loaded=miosaurusPresetLoadedSlot();
  const uint8_t browse=miosaurusPresetBrowseSlot();
  const bool edited=miosaurusPresetEdited();
  char browseName[MIO_PRESET_NAME_MAX+1];
  const bool browseExists=miosaurusPresetGetSlotName(browse,browseName,sizeof(browseName));

  if(gPresetPanelTitle) {
    char b[96];
    snprintf(b,sizeof(b),"LOADED P%03u%s   TARGET P%03u",
             (unsigned)loaded,edited?"*":"",(unsigned)browse);
    lv_label_set_text(gPresetPanelTitle,b);
  }

  if(gPresetNameField) {
    if(gPresetNameSlot!=browse) {
      gPresetNameDirty=false;
      gPresetNameSlot=browse;
    }
    if(!gPresetNameDirty) {
      if(browseExists) {
        strncpy(gPresetEditName,browseName,sizeof(gPresetEditName)-1);
      } else if(browse==loaded) {
        strncpy(gPresetEditName,miosaurusPresetLoadedName(),sizeof(gPresetEditName)-1);
      } else {
        strncpy(gPresetEditName,"INIT PATCH",sizeof(gPresetEditName)-1);
      }
      gPresetEditName[sizeof(gPresetEditName)-1]='\0';
      lv_textarea_set_text(gPresetNameField,gPresetEditName);
    }
  }

  if(gPresetPanelStatus) {
    const char *s=miosaurusPresetStatus();
    char b[96];
    snprintf(b,sizeof(b),"%s%s%s",
             browseExists?browseName:"EMPTY",
             edited?"   | EDITED":"",
             (s && s[0])?"   | ":"");
    if(s && s[0]) strncat(b,s,sizeof(b)-strlen(b)-1);
    lv_label_set_text(gPresetPanelStatus,b);
  }
}


static void closeNameEditor() {
  if(!gNameOverlay) return;
  if(gNameTextArea) {
    strncpy(gPresetEditName,lv_textarea_get_text(gNameTextArea),
            sizeof(gPresetEditName)-1);
    gPresetEditName[sizeof(gPresetEditName)-1]='\0';
    gPresetNameDirty=true;
  }
  lv_obj_add_flag(gNameOverlay,LV_OBJ_FLAG_HIDDEN);
  if(gPresetPanel) lv_obj_clear_flag(gPresetPanel,LV_OBJ_FLAG_HIDDEN);
  if(gPresetNameField) lv_textarea_set_text(gPresetNameField,gPresetEditName);
}

static void nameEditorEvent(lv_event_t *e) {
  const lv_event_code_t code=lv_event_get_code(e);

  if(code==LV_EVENT_VALUE_CHANGED && lv_event_get_target(e)==gNameTextArea) {
    const char *cur=lv_textarea_get_text(gNameTextArea);
    if(!cur) return;

    const size_t oldLen=strlen(gKeyboardAcceptedText);
    const size_t newLen=strlen(cur);
    const uint32_t now=millis();

    // GT911 can occasionally produce a second press/release for one physical
    // tap. Reject only the very specific case "same character appended twice
    // in very short succession"; normal fast typing of different chars stays.
    bool duplicate=false;
    if(newLen==oldLen+1 && oldLen>0 &&
       strncmp(cur,gKeyboardAcceptedText,oldLen)==0 &&
       (uint32_t)(now-gLastKeyboardAcceptMs)<KEYBOARD_DUP_GUARD_MS) {
      const char added=cur[newLen-1];
      const char previous=gKeyboardAcceptedText[oldLen-1];
      if(added==previous) duplicate=true;
    }

    if(duplicate) {
      lv_textarea_set_text(gNameTextArea,gKeyboardAcceptedText);
      lv_textarea_set_cursor_pos(gNameTextArea,LV_TEXTAREA_CURSOR_LAST);
      return;
    }

    strncpy(gKeyboardAcceptedText,cur,sizeof(gKeyboardAcceptedText)-1);
    gKeyboardAcceptedText[sizeof(gKeyboardAcceptedText)-1]='\0';
    gLastKeyboardAcceptMs=now;
    return;
  }

  if(code==LV_EVENT_READY || code==LV_EVENT_CANCEL) closeNameEditor();
}

static void actuallyOpenNameEditor() {
  if(!gNameOverlay) return;
  if(gPresetNameField) {
    strncpy(gPresetEditName,lv_textarea_get_text(gPresetNameField),
            sizeof(gPresetEditName)-1);
    gPresetEditName[sizeof(gPresetEditName)-1]='\0';
  }

  strncpy(gKeyboardAcceptedText,gPresetEditName,sizeof(gKeyboardAcceptedText)-1);
  gKeyboardAcceptedText[sizeof(gKeyboardAcceptedText)-1]='\0';
  gLastKeyboardAcceptMs=0;

  lv_textarea_set_text(gNameTextArea,gPresetEditName);
  lv_obj_add_flag(gPresetPanel,LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(gNameOverlay,LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(gNameKeyboard,gNameTextArea);
  lv_group_focus_obj(gNameTextArea);
}

static void openNameEditor(lv_event_t *e) {
  if(lv_event_get_code(e)!=LV_EVENT_RELEASED) return;
  const uint32_t now=millis();
  if(gLastNameFieldReleaseMs &&
     (uint32_t)(now-gLastNameFieldReleaseMs)<NAME_FIELD_GUARD_MS) return;
  gLastNameFieldReleaseMs=now;

  // Delay overlay creation until the original touch has fully disappeared.
  // This prevents the release/click from landing on a keyboard key underneath.
  gNameOpenPending=true;
  gNameOpenAfterMs=now+90;
}

static void presetConfirmYes(lv_event_t *e) {
  (void)e;
  GuiPresetConfirm action=gPresetConfirm;
  closePresetConfirm();
  if(action==GUI_PRESET_CONFIRM_LOAD) {
    if(miosaurusPresetLoadBrowse()) gPresetNameDirty=false;
  } else if(action==GUI_PRESET_CONFIRM_SAVEAS) {
    if(miosaurusPresetSaveAsBrowse(gPresetEditName)) gPresetNameDirty=false;
  }
  refreshPresetHeader();
  refreshPresetPanel();
}

static void presetConfirmNo(lv_event_t *e) {
  (void)e;
  closePresetConfirm();
  refreshPresetPanel();
}

static void showPresetConfirm(GuiPresetConfirm action,const char *msg) {
  gPresetConfirm=action;
  if(gPresetConfirmLabel) {
    lv_label_set_text(gPresetConfirmLabel,msg);
    lv_obj_clear_flag(gPresetConfirmLabel,LV_OBJ_FLAG_HIDDEN);
  }
  if(gPresetConfirmYes) lv_obj_clear_flag(gPresetConfirmYes,LV_OBJ_FLAG_HIDDEN);
  if(gPresetConfirmNo) lv_obj_clear_flag(gPresetConfirmNo,LV_OBJ_FLAG_HIDDEN);
}

static bool presetPanelArrowMayFire() {
  const uint32_t now=millis();
  if(gLastPresetPanelArrowMs &&
     (uint32_t)(now-gLastPresetPanelArrowMs)<PRESET_PANEL_ARROW_GUARD_MS) {
    return false;
  }
  gLastPresetPanelArrowMs=now;
  return true;
}

static void presetSlotPrev(lv_event_t *e) {
  (void)e;
  if(!presetPanelArrowMayFire()) return;
  closePresetConfirm();
  gPresetNameDirty=false;
  miosaurusPresetBrowsePrev();
  refreshPresetPanel();
}
static void presetSlotNext(lv_event_t *e) {
  (void)e;
  if(!presetPanelArrowMayFire()) return;
  closePresetConfirm();
  gPresetNameDirty=false;
  miosaurusPresetBrowseNext();
  refreshPresetPanel();
}
static void presetLoad(lv_event_t *e) {
  (void)e;
  closePresetConfirm();
  if(!miosaurusPresetSlotExists(miosaurusPresetBrowseSlot())) {
    miosaurusPresetLoadBrowse(); // sets EMPTY status without changing the loaded preset
    refreshPresetPanel();
    return;
  }
  if(miosaurusPresetEdited()) {
    char b[64];
    snprintf(b,sizeof(b),"LOAD P%03u? EDITS LOST",
             (unsigned)miosaurusPresetBrowseSlot());
    showPresetConfirm(GUI_PRESET_CONFIRM_LOAD,b);
  } else {
    if(miosaurusPresetLoadBrowse()) gPresetNameDirty=false;
    refreshPresetHeader();
    refreshPresetPanel();
  }
}
static void presetSave(lv_event_t *e) {
  (void)e;
  closePresetConfirm();
  if(miosaurusPresetSaveLoaded(gPresetEditName)) gPresetNameDirty=false;
  refreshPresetHeader();
  refreshPresetPanel();
}
static void presetSaveAs(lv_event_t *e) {
  (void)e;
  closePresetConfirm();
  uint8_t browse=miosaurusPresetBrowseSlot();
  uint8_t loaded=miosaurusPresetLoadedSlot();
  if(browse==loaded || !miosaurusPresetSlotExists(browse)) {
    if(miosaurusPresetSaveAsBrowse(gPresetEditName)) gPresetNameDirty=false;
    refreshPresetHeader();
    refreshPresetPanel();
    return;
  }
  char b[64];
  snprintf(b,sizeof(b),"OVERWRITE P%03u?",(unsigned)browse);
  showPresetConfirm(GUI_PRESET_CONFIRM_SAVEAS,b);
}

static void presetClose(lv_event_t *e) {
  (void)e;
  closePresetConfirm();
  gPresetPanelOpen=false;
  if(gPresetPanel) lv_obj_add_flag(gPresetPanel,LV_OBJ_FLAG_HIDDEN);
  if(gPageArea) lv_obj_clear_flag(gPageArea,LV_OBJ_FLAG_HIDDEN);
}

static void onPresetManager(lv_event_t *e) {
  (void)e;
  gPresetPanelOpen=!gPresetPanelOpen;
  closePresetConfirm();
  if(gPresetPanelOpen) {
    if(gPageArea) lv_obj_add_flag(gPageArea,LV_OBJ_FLAG_HIDDEN);
    if(gPresetPanel) lv_obj_clear_flag(gPresetPanel,LV_OBJ_FLAG_HIDDEN);
    refreshPresetPanel();
  } else {
    if(gPresetPanel) lv_obj_add_flag(gPresetPanel,LV_OBJ_FLAG_HIDDEN);
    if(gPageArea) lv_obj_clear_flag(gPageArea,LV_OBJ_FLAG_HIDDEN);
  }
}

static void createPresetPanel() {
  gPresetPanel=lv_obj_create(gRoot);
  lv_obj_set_pos(gPresetPanel,4,119);
  lv_obj_set_size(gPresetPanel,472,133);
  lv_obj_set_style_bg_color(gPresetPanel,COL_BG,0);
  lv_obj_set_style_border_color(gPresetPanel,COL_BORDER,0);
  lv_obj_set_style_border_width(gPresetPanel,1,0);
  lv_obj_set_style_radius(gPresetPanel,6,0);
  lv_obj_set_style_pad_all(gPresetPanel,0,0);
  lv_obj_clear_flag(gPresetPanel,LV_OBJ_FLAG_SCROLLABLE);

  gPresetPanelTitle=lv_label_create(gPresetPanel);
  lv_obj_set_pos(gPresetPanelTitle,9,6);
  lv_obj_set_width(gPresetPanelTitle,450);
  lv_obj_set_style_text_color(gPresetPanelTitle,COL_TEXT,0);

  gPresetNameField=lv_textarea_create(gPresetPanel);
  lv_obj_set_pos(gPresetNameField,8,27);
  lv_obj_set_size(gPresetNameField,220,31);
  lv_textarea_set_one_line(gPresetNameField,true);
  lv_obj_add_flag(gPresetNameField,LV_OBJ_FLAG_PRESS_LOCK);
  lv_textarea_set_max_length(gPresetNameField,MIO_PRESET_NAME_MAX);
  lv_textarea_set_text(gPresetNameField,"INIT PATCH");
  lv_obj_add_event_cb(gPresetNameField,openNameEditor,LV_EVENT_RELEASED,nullptr);

  makeButton(gPresetPanel,"<",236,27,34,31,presetSlotPrev,0);
  makeButton(gPresetPanel,">",274,27,34,31,presetSlotNext,0);
  makeButton(gPresetPanel,"LOAD",313,27,49,31,presetLoad,0);
  makeButton(gPresetPanel,"SAVE",366,27,47,31,presetSave,0);
  makeButton(gPresetPanel,"AS",417,27,43,31,presetSaveAs,0);

  gPresetPanelStatus=lv_label_create(gPresetPanel);
  lv_obj_set_pos(gPresetPanelStatus,9,72);
  lv_obj_set_width(gPresetPanelStatus,372);
  lv_obj_set_height(gPresetPanelStatus,20);
  lv_label_set_long_mode(gPresetPanelStatus,LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_color(gPresetPanelStatus,COL_ACC,0);

  makeButton(gPresetPanel,"CLOSE",392,68,68,28,presetClose,0);

  gPresetConfirmLabel=lv_label_create(gPresetPanel);
  lv_obj_set_pos(gPresetConfirmLabel,9,105);
  lv_obj_set_width(gPresetConfirmLabel,275);
  lv_obj_set_style_text_color(gPresetConfirmLabel,COL_TEXT,0);

  gPresetConfirmYes=makeButton(gPresetPanel,"YES",300,98,74,30,presetConfirmYes,0);
  gPresetConfirmNo=makeButton(gPresetPanel,"NO",383,98,74,30,presetConfirmNo,0);

  closePresetConfirm();
  lv_obj_add_flag(gPresetPanel,LV_OBJ_FLAG_HIDDEN);

  // Full-width touch keyboard overlay for preset names.
  gNameOverlay=lv_obj_create(gRoot);
  lv_obj_set_pos(gNameOverlay,0,80);
  lv_obj_set_size(gNameOverlay,480,192);
  lv_obj_set_style_bg_color(gNameOverlay,COL_BG,0);
  lv_obj_set_style_border_width(gNameOverlay,0,0);
  lv_obj_set_style_pad_all(gNameOverlay,2,0);
  lv_obj_clear_flag(gNameOverlay,LV_OBJ_FLAG_SCROLLABLE);

  gNameTextArea=lv_textarea_create(gNameOverlay);
  lv_obj_set_pos(gNameTextArea,4,2);
  lv_obj_set_size(gNameTextArea,472,36);
  lv_textarea_set_one_line(gNameTextArea,true);
  lv_textarea_set_max_length(gNameTextArea,MIO_PRESET_NAME_MAX);
  lv_obj_add_event_cb(gNameTextArea,nameEditorEvent,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_add_event_cb(gNameTextArea,nameEditorEvent,LV_EVENT_READY,nullptr);
  lv_obj_add_event_cb(gNameTextArea,nameEditorEvent,LV_EVENT_CANCEL,nullptr);

  gNameKeyboard=lv_keyboard_create(gNameOverlay);
  lv_obj_set_pos(gNameKeyboard,4,42);
  lv_obj_set_size(gNameKeyboard,472,146);
  lv_keyboard_set_mode(gNameKeyboard,LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(gNameKeyboard,gNameTextArea);
  lv_obj_add_event_cb(gNameKeyboard,nameEditorEvent,LV_EVENT_READY,nullptr);
  lv_obj_add_event_cb(gNameKeyboard,nameEditorEvent,LV_EVENT_CANCEL,nullptr);

  lv_obj_add_flag(gNameOverlay,LV_OBJ_FLAG_HIDDEN);
}

static void onMode(lv_event_t *e) {
  (void)e;
  miosaurusPerformanceTogglePara();
}
static void onLegato(lv_event_t *e) {
  (void)e;
  miosaurusPerformanceToggleLegato();
}
static void onPriority(lv_event_t *e) {
  (void)e;
  miosaurusPerformanceCyclePriority();
}

static const char *learnTargetName(uint8_t target) {
  if(target < PARAM_COUNT) return miosaurusStateDef((MioParamId)target).name;
  switch(target) {
    case MIO_LT_LEGATO: return "LEGATO";
    case MIO_LT_PRIORITY: return "PRIORITY";
    case MIO_LT_PARA: return "PARA";
    case MIO_LT_VEL_ROUTE: return "VEL ROUTE";
    case MIO_LT_AT_ROUTE: return "AT ROUTE";
    case MIO_LT_VEL_AMT: return "VEL AMT";
    case MIO_LT_AT_AMT: return "AT AMT";
    case MIO_LT_VEL_CURVE: return "VEL CURVE";
    default: return "---";
  }
}

static uint8_t nextLearnTarget(uint8_t start, int dir) {
  int t=start;
  for(int i=0;i<MIO_LEARN_TARGET_COUNT;i++) {
    t += dir;
    if(t < 0) t=MIO_LEARN_TARGET_COUNT-1;
    if(t >= MIO_LEARN_TARGET_COUNT) t=0;
    if(miosaurusMidiLearnAllowed(gLearnBank,(uint8_t)t)) return (uint8_t)t;
  }
  return start;
}

static void refreshLearnOverlay() {
  if(!gLearnOverlay) return;

  if(gMidiChannelButton) {
    lv_obj_t *l=lv_obj_get_child(gMidiChannelButton,0);
    if(l) lv_label_set_text(l,miosaurusMidiReceiveChannelName());
  }
  if(!miosaurusMidiLearnAllowed(gLearnBank,gLearnTarget))
    gLearnTarget=nextLearnTarget(gLearnTarget,1);

  if(gLearnBankButton) {
    lv_obj_t *l=lv_obj_get_child(gLearnBankButton,0);
    if(l) lv_label_set_text(l,gLearnBank?"BANK B1":"BANK B0");
  }

  if(gLearnTargetLabel) {
    char b[64];
    snprintf(b,sizeof(b),"%02u  %s",(unsigned)gLearnTarget,learnTargetName(gLearnTarget));
    lv_label_set_text(gLearnTargetLabel,b);
  }

  if(gLearnCcLabel) {
    const uint8_t cc=miosaurusMidiEffectiveCc(gLearnBank,gLearnTarget);
    char b[40];
    if(cc==0xFF) snprintf(b,sizeof(b),"CC: --");
    else snprintf(b,sizeof(b),"CC: %u",(unsigned)cc);
    lv_label_set_text(gLearnCcLabel,b);
  }

  if(gLearnStatusLabel) {
    char b[96];
    if(miosaurusMidiLearnArmed()) {
      snprintf(b,sizeof(b),"LEARN ARMED - move a MIDI CC");
    } else {
      const uint8_t last=miosaurusMidiLearnLastCc();
      if(last!=0xFF) snprintf(b,sizeof(b),"LEARNED CC %u",(unsigned)last);
      else snprintf(b,sizeof(b),"Select target, press LEARN, move controller");
    }
    lv_label_set_text(gLearnStatusLabel,b);
  }
}

static void midiChannelCycle(lv_event_t *e) {
  (void)e;
  // V0.13C FIX1: The GT911/LVGL path can occasionally emit a second
  // RELEASE for one physical tap. Keep this guard local to MIDI channel
  // selection so sliders and the rest of the touch UI remain unchanged.
  const uint32_t now=millis();
  if(gLastMidiChannelCycleMs &&
     (uint32_t)(now-gLastMidiChannelCycleMs)<MIDI_CHANNEL_REFIRE_GUARD_MS) return;
  gLastMidiChannelCycleMs=now;

  uint8_t ch=miosaurusMidiReceiveChannel();
  if(ch==0) ch=1;
  else if(ch>=16) ch=0;
  else ++ch;
  miosaurusMidiSetReceiveChannel(ch);

  if(gMidiChannelButton) {
    lv_obj_t *l=lv_obj_get_child(gMidiChannelButton,0);
    if(l) lv_label_set_text(l,miosaurusMidiReceiveChannelName());
  }
}

static void learnBankToggle(lv_event_t *e) {
  (void)e;
  miosaurusMidiLearnCancel();

  // V0.14B FIX5: the MIDI page must switch the real performance CC bank,
  // not only its local display/learn-bank variable.  Previously the button
  // could show BANK B1 while incoming CCs were still resolved through B0.
  const uint8_t newBank = miosaurusPerformanceCcBank() ? 0 : 1;
  miosaurusPerformanceSetCcBank(newBank);
  gLearnBank = miosaurusPerformanceCcBank();

  if(!miosaurusMidiLearnAllowed(gLearnBank,gLearnTarget))
    gLearnTarget=nextLearnTarget(gLearnTarget,1);
  refreshLearnOverlay();
}

static void learnTargetPrev(lv_event_t *e) {
  (void)e;
  miosaurusMidiLearnCancel();
  gLearnTarget=nextLearnTarget(gLearnTarget,-1);
  refreshLearnOverlay();
}
static void learnTargetNext(lv_event_t *e) {
  (void)e;
  miosaurusMidiLearnCancel();
  gLearnTarget=nextLearnTarget(gLearnTarget,1);
  refreshLearnOverlay();
}
static void learnArm(lv_event_t *e) {
  (void)e;
  miosaurusMidiLearnArm(gLearnBank,gLearnTarget);
  refreshLearnOverlay();
}
static void learnClear(lv_event_t *e) {
  (void)e;
  miosaurusMidiLearnCancel();
  miosaurusMidiClearLearn(gLearnBank,gLearnTarget);
  refreshLearnOverlay();
}
static void learnResetBank(lv_event_t *e) {
  (void)e;
  miosaurusMidiLearnCancel();
  miosaurusMidiResetLearnBank(gLearnBank);
  refreshLearnOverlay();
}
static void learnClose(lv_event_t *e) {
  (void)e;
  miosaurusMidiLearnCancel();
  // FIX2: CLOSE is modal. Consume the complete GT911 gesture before the
  // underlying MAIN page can receive input again; otherwise the same/ghost
  // touch can land on CUTOFF after the overlay is hidden.
  Board::suppressTouchUntilReleased(90);
  if(gLearnOverlay) lv_obj_add_flag(gLearnOverlay,LV_OBJ_FLAG_HIDDEN);
}

static void openMidiLearnOverlay() {
  if(!gLearnOverlay) return;
  gLearnBank=miosaurusPerformanceCcBank();
  if(!miosaurusMidiLearnAllowed(gLearnBank,gLearnTarget)) {
    gLearnTarget=0;
    if(!miosaurusMidiLearnAllowed(gLearnBank,gLearnTarget))
      gLearnTarget=nextLearnTarget(gLearnTarget,1);
  }
  refreshLearnOverlay();
  lv_obj_clear_flag(gLearnOverlay,LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(gLearnOverlay);
}

static void createMidiLearnOverlay() {
  gLearnOverlay=lv_obj_create(gRoot);
  lv_obj_set_pos(gLearnOverlay,0,0);
  lv_obj_set_size(gLearnOverlay,480,272);
  lv_obj_set_style_bg_color(gLearnOverlay,COL_BG,0);
  lv_obj_set_style_bg_opa(gLearnOverlay,LV_OPA_COVER,0);
  lv_obj_set_style_border_width(gLearnOverlay,0,0);
  lv_obj_set_style_pad_all(gLearnOverlay,8,0);
  lv_obj_clear_flag(gLearnOverlay,LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title=lv_label_create(gLearnOverlay);
  lv_label_set_text(title,"MIDI");
  lv_obj_set_pos(title,10,8);
  lv_obj_set_style_text_color(title,COL_ACC,0);

  gMidiChannelButton=makeButton(gLearnOverlay,"CH 01",10,38,92,34,midiChannelCycle,0);
  gLearnBankButton=makeButton(gLearnOverlay,"BANK B0",110,38,105,34,learnBankToggle,0);
  makeButton(gLearnOverlay,"<",224,38,42,34,learnTargetPrev,0);
  makeButton(gLearnOverlay,">",274,38,42,34,learnTargetNext,0);

  gLearnTargetLabel=lv_label_create(gLearnOverlay);
  lv_obj_set_pos(gLearnTargetLabel,10,86);
  lv_obj_set_width(gLearnTargetLabel,300);
  lv_obj_set_style_text_color(gLearnTargetLabel,COL_TEXT,0);

  gLearnCcLabel=lv_label_create(gLearnOverlay);
  lv_obj_set_pos(gLearnCcLabel,324,86);
  lv_obj_set_width(gLearnCcLabel,130);
  lv_obj_set_style_text_align(gLearnCcLabel,LV_TEXT_ALIGN_RIGHT,0);
  lv_obj_set_style_text_color(gLearnCcLabel,COL_ACC,0);

  makeButton(gLearnOverlay,"LEARN",10,122,92,36,learnArm,0);
  makeButton(gLearnOverlay,"CLEAR",112,122,92,36,learnClear,0);
  makeButton(gLearnOverlay,"RESET BANK",214,122,120,36,learnResetBank,0);
  makeButton(gLearnOverlay,"CLOSE",344,122,110,36,learnClose,0);

  gLearnStatusLabel=lv_label_create(gLearnOverlay);
  lv_obj_set_pos(gLearnStatusLabel,10,178);
  lv_obj_set_width(gLearnStatusLabel,444);
  lv_obj_set_height(gLearnStatusLabel,24);
  lv_label_set_long_mode(gLearnStatusLabel,LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_color(gLearnStatusLabel,COL_MUTED,0);

  lv_obj_t *hint=lv_label_create(gLearnOverlay);
  lv_label_set_text(hint,"MIDI channel + CC bank + MIDI learn");
  lv_obj_set_pos(hint,10,218);
  lv_obj_set_width(hint,444);
  lv_obj_set_style_text_color(hint,COL_MUTED,0);

  lv_obj_add_flag(gLearnOverlay,LV_OBJ_FLAG_HIDDEN);
}

static void onMidiOpen(lv_event_t *e) {
  (void)e;
  openMidiLearnOverlay();
}

static void refreshPerformanceHeader() {
  if(gModeLabel)
    lv_label_set_text(gModeLabel,miosaurusPerformancePara()?"PARA":"MONO");
  if(gLegatoButton) {
    lv_obj_t *l=lv_obj_get_child(gLegatoButton,0);
    if(l) lv_label_set_text(l,miosaurusPerformanceLegato()?"LEG ON":"LEGATO");
    lv_obj_set_style_bg_color(gLegatoButton,
      miosaurusPerformanceLegato()?COL_ACC:COL_PANEL,0);
    if(l) lv_obj_set_style_text_color(l,
      miosaurusPerformanceLegato()?lv_color_hex(0x241208):COL_TEXT,0);
  }
  if(gPriorityButton) {
    lv_obj_t *l=lv_obj_get_child(gPriorityButton,0);
    char b[24];
    snprintf(b,sizeof(b),"PRI %s",miosaurusPerformancePriorityName());
    if(l) lv_label_set_text(l,b);
  }
  if(gMidiButton) {
    // V0.13C FIX1: Main GUI stays neutral; bank selection remains visible
    // inside the MIDI page only.
    lv_obj_t *l=lv_obj_get_child(gMidiButton,0);
    if(l) lv_label_set_text(l,"MIDI");
  }
}

void miosaurusGuiCreate(lv_obj_t *parent) {
  // V0.8: GUI mirrors the real z33 parameter-state defaults/ranges.
  for(int i=0;i<PARAM_COUNT;i++) gValue[i]=miosaurusStateGetNormalized((MioParamId)i);

  gRoot=parent?parent:lv_scr_act();
  lv_obj_set_style_bg_color(gRoot,COL_BG,0);
  lv_obj_set_style_bg_opa(gRoot,LV_OPA_COVER,0);
  lv_obj_set_style_border_width(gRoot,0,0);
  lv_obj_clear_flag(gRoot,LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title=lv_label_create(gRoot);
  lv_label_set_text(title,"RealTimeAudioLab MIOSAURUS");
  lv_obj_set_pos(title,9,4);
  lv_obj_set_style_text_color(title,COL_ACC,0);

  lv_obj_t *prev=makeButton(gRoot,"<",6,26,36,30,onPresetPrev,-1);
  (void)prev;
  gPresetLabel=lv_label_create(gRoot);
  lv_label_set_text(gPresetLabel,"P1");
  lv_obj_set_pos(gPresetLabel,50,34);
  lv_obj_set_width(gPresetLabel,380);
  lv_obj_set_style_text_color(gPresetLabel,COL_TEXT,0);
  makeButton(gRoot,">",438,26,36,30,onPresetNext,1);

  makeButton(gRoot,"MODE",8,60,74,28,onMode,0);
  gModeLabel=lv_label_create(gRoot);
  lv_label_set_text(gModeLabel,miosaurusPerformancePara()?"PARA":"MONO");
  lv_obj_set_pos(gModeLabel,88,68);
  lv_obj_set_style_text_color(gModeLabel,COL_TEXT,0);

  gLegatoButton=makeButton(gRoot,"LEGATO",145,60,75,28,onLegato,0);
  gPriorityButton=makeButton(gRoot,"PRI LAST",226,60,82,28,onPriority,0);
  // V0.13A: one normal press opens the MIDI page immediately.
  gMidiButton=makeButton(gRoot,"MIDI",319,60,72,28,onMidiOpen,0);
  gPresetButton=makeButton(gRoot,"PRESET",395,60,78,28,onPresetManager,0);

  for(int i=0;i<7;i++) {
    gPageButtons[i]=makeButton(gRoot,PAGE_NAMES[i],4+i*68,92,64,25,onPage,i);
  }

  gPageArea=lv_obj_create(gRoot);
  lv_obj_set_pos(gPageArea,4,119);
  lv_obj_set_size(gPageArea,472,133);
  lv_obj_set_style_bg_color(gPageArea,COL_BG,0);
  lv_obj_set_style_radius(gPageArea,5,0);
  lv_obj_set_style_border_width(gPageArea,0,0);
  lv_obj_set_style_pad_all(gPageArea,0,0);
  lv_obj_clear_flag(gPageArea,LV_OBJ_FLAG_SCROLLABLE);

  createPresetPanel();
  createMidiLearnOverlay();

  gInfo=lv_label_create(gRoot);
  lv_obj_set_pos(gInfo,9,254);
  lv_obj_set_width(gInfo,462);
  lv_label_set_long_mode(gInfo,LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(gInfo,COL_MUTED,0);
  lv_label_set_text(gInfo,"LAST: --");

  buildPage();
}

void miosaurusGuiService() {
  if(gNameOpenPending &&
     (int32_t)(millis()-gNameOpenAfterMs)>=0) {
    gNameOpenPending=false;
    actuallyOpenNameEditor();
  }

  flushPending();
  refreshPerformanceHeader();

  // V0.14B FIX5: keep the MIDI overlay bank display synchronized with the
  // actual performance bank, including external CC32 bank changes.
  if(gLearnOverlay && !lv_obj_has_flag(gLearnOverlay,LV_OBJ_FLAG_HIDDEN)) {
    const uint8_t activeBank=miosaurusPerformanceCcBank();
    if(activeBank!=gLearnBank) {
      gLearnBank=activeBank;
      if(!miosaurusMidiLearnAllowed(gLearnBank,gLearnTarget))
        gLearnTarget=nextLearnTarget(gLearnTarget,1);
      refreshLearnOverlay();
    }
  }

  const uint32_t learnRev=miosaurusMidiLearnRevision();
  if(learnRev!=gLastLearnRevision) {
    gLastLearnRevision=learnRev;
    refreshLearnOverlay();
  }

  uint32_t presetRev=miosaurusPresetRevision();
  if(presetRev!=gLastPresetRevision) {
    gLastPresetRevision=presetRev;
    refreshPresetHeader();
    refreshPresetPanel();
  }

  // State/MIDI/Preset -> GUI synchronization. LVGL is touched only here
  // from the Core0 GUI task. Limit each pass to keep UI latency bounded.
  for (int n = 0; n < 12; ++n) {
    MioParamId id;
    float normalized01;
    if (!miosaurusStateConsumeGuiDirty(id, normalized01)) break;
    miosaurusGuiSetParam(id, normalized01);
  }
}

void miosaurusGuiSetParam(MioParamId id, float normalized01) {
  if(id>=PARAM_COUNT) return;
  gValue[id]=constrain(normalized01,0.0f,1.0f);
  for(int i=0;i<6;i++) {
    if(gSliders[i] && gControlIds[i]==id) {
      lv_slider_set_value(gSliders[i],(int)lroundf(gValue[id]*127.0f),LV_ANIM_OFF);
      refreshSliderValue(i);
    }
  }
}

float miosaurusGuiGetParam(MioParamId id) {
  if(id>=PARAM_COUNT) return 0.0f;
  return gValue[id];
}

void miosaurusGuiSetPreset(uint8_t slot, const char *name, bool edited) {
  (void)slot; (void)name; (void)edited;
  refreshPresetHeader();
  refreshPresetPanel();
}

void miosaurusGuiOnParamChanged(MioParamId id, float normalized01) {
  // GUI -> z33 state bridge. Never write DSP state directly from an LVGL callback.
  miosaurusStateQueueGuiNormalized(id, normalized01);
}
