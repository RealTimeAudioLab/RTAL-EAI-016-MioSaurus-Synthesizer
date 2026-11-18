/*
  RTAL MIOSAURUS audio engine
  First audible z33 DSP migration.

  Source basis:
    - MIOSAURUS z33 DSP topology and parameter semantics
    - JC4827W543 I2S hardware layer

  V0.11 performance extensions:
    - monophonic note-on/off only
    - no z33 CC bank / MIDI Learn / presets yet
    - no Para2 / legato / priority layer yet
    - MIDI-clock sync paths use free-running values until the z33 MIDI layer is ported

  Platform audio:
    32 kHz / 256 frames, matching the hardware-confirmed JC4827W543 platform.
*/

#include "Z_MiosaurusAudio.h"
#include "Z_MiosaurusState.h"
#include "Z_MiosaurusGui.h"
#include "Z_MiosaurusPerformance.h"

#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <driver/i2s_std.h>

namespace {

static constexpr float SAMPLE_RATE = 32000.0f;
static constexpr int AUDIO_BLOCK_SAMPLES = 256;

static constexpr int PIN_BCLK = 16;
static constexpr int PIN_LRCK = 15;
static constexpr int PIN_DOUT = 7;

static i2s_chan_handle_t sTx = nullptr;

enum AudioVoiceCmdType : uint8_t {
  AVC_MONO,
  AVC_PARA,
  AVC_RELEASE,
  AVC_RESET_LFO,
  AVC_RESET_FX
};

struct AudioVoiceCmd {
  AudioVoiceCmdType type;
  int8_t n1;
  int8_t n2;
  int8_t sub;
  uint8_t v1;
  uint8_t v2;
  bool retrigger;
  bool singleUnison;
};

static QueueHandle_t sVoiceQueue = nullptr;

static volatile uint32_t sDiagBlocks = 0;
static volatile uint32_t sDiagRenderUsTotal = 0;
static volatile uint32_t sDiagRenderUsMax = 0;
static volatile uint32_t sDiagRenderOver8ms = 0;
static volatile uint32_t sDiagI2sErrors = 0;
static volatile uint32_t sDiagShortWrites = 0;
static volatile uint32_t sDiagVoiceQueueDrops = 0;

static inline float clampfLocal(float x, float lo, float hi) {
  if (!isfinite(x)) return lo;
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

static inline float clampSafe(float x, float lo, float hi, float fallback) {
  if (!isfinite(x)) return fallback;
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

static inline float fastSoftClip(float x) {
  x = clampfLocal(x, -3.0f, 3.0f);
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static inline int32_t floatToI32(float x) {
  x = clampfLocal(x, -1.0f, 1.0f);
  return (int32_t)(x * 2147483647.0f);
}

static inline float midiNoteToFreq(uint8_t note) {
  return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

static inline float poly_blep(float t, float dt) {
  if (dt <= 0.0f) return 0.0f;
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

class ADSR {
public:
  enum State : uint8_t { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };

  void setSampleRate(float sr) { sampleRate = sr; updateCoeffs(); }
  void setAttack(float sec) {
    attack = clampSafe(sec, 0.0005f, 10.0f, 0.01f);
    attackCoeff = timeToCoeffAttack(attack);
  }
  void setDecay(float sec) {
    decay = clampSafe(sec, 0.0005f, 10.0f, 0.20f);
    decayCoeff = timeToCoeffDecay(decay);
  }
  void setSustain(float s) { sustain = clampfLocal(s, 0.0f, 1.0f); }
  void setRelease(float sec) {
    release = clampSafe(sec, 0.0005f, 10.0f, 0.15f);
    releaseCoeff = timeToCoeffRelease(release);
  }

  void noteOn() { state = ATTACK; }
  void noteOff() { if (state != IDLE) state = RELEASE; }
  void reset() { value = 0.0f; state = IDLE; }

  float process() {
    switch (state) {
      case IDLE:
        value = 0.0f;
        break;
      case ATTACK:
        value += (1.0f - value) * attackCoeff;
        if (value >= 0.9995f) { value = 1.0f; state = DECAY; }
        break;
      case DECAY:
        value += (sustain - value) * decayCoeff;
        if (fabsf(value - sustain) <= 0.0005f) {
          value = sustain;
          state = SUSTAIN;
        }
        break;
      case SUSTAIN:
        value = sustain;
        break;
      case RELEASE:
        value += (0.0f - value) * releaseCoeff;
        if (value <= 0.0005f) { value = 0.0f; state = IDLE; }
        break;
    }
    return value;
  }

private:
  float timeToCoeffAttack(float sec) const {
    float samples = clampSafe(sec * sampleRate, 1.0f, 10000000.0f, 100.0f);
    return clampSafe(8.0f / samples, 0.000001f, 1.0f, 0.01f);
  }
  float timeToCoeffDecay(float sec) const {
    float samples = clampSafe(sec * sampleRate, 1.0f, 10000000.0f, 100.0f);
    return clampSafe(5.5f / samples, 0.000001f, 1.0f, 0.01f);
  }
  float timeToCoeffRelease(float sec) const {
    float samples = clampSafe(sec * sampleRate, 1.0f, 10000000.0f, 100.0f);
    return clampSafe(4.5f / samples, 0.000001f, 1.0f, 0.01f);
  }
  void updateCoeffs() {
    attackCoeff = timeToCoeffAttack(attack);
    decayCoeff = timeToCoeffDecay(decay);
    releaseCoeff = timeToCoeffRelease(release);
  }

  float sampleRate = SAMPLE_RATE;
  float attack = 0.01f;
  float decay = 0.20f;
  float sustain = 0.80f;
  float release = 0.15f;
  float attackCoeff = 0.01f;
  float decayCoeff = 0.01f;
  float releaseCoeff = 0.01f;
  float value = 0.0f;
  State state = IDLE;
};

enum Waveform : uint8_t { WAVE_SAW, WAVE_SQUARE };

class Oscillator {
public:
  void setSampleRate(float sr) {
    sampleRate = sr;
    invSampleRate = 1.0f / max(1.0f, sr);
    updatePhaseInc();
  }
  void setFrequency(float hz) {
    frequency = clampSafe(hz, 1.0f, sampleRate * 0.45f, 110.0f);
    updatePhaseInc();
  }
  void setWaveform(Waveform wf) { wave = wf; morph = (wf == WAVE_SQUARE) ? 1.0f : 0.0f; }
  void setMorph(float m) { morph = clampfLocal(m, 0.0f, 1.0f); }
  void setPulseWidth(float pw) { pulseWidth = clampfLocal(pw, 0.05f, 0.95f); }
  void resetPhase(float p = 0.0f) {
    phase = p - floorf(p);
    if (phase < 0.0f) phase += 1.0f;
  }

  float process() {
    const float t = phase;
    const float dt = phaseInc;
    float out = 0.0f;

    // V0.14A: continuous 128-step SAW <-> SQR morph.
    // Preserve the cheap legacy endpoint paths so pure SAW and pure SQR cost
    // essentially the same as before; only intermediate positions render both
    // band-limited shapes and crossfade them.
    if (morph <= 0.00001f) {
      out = 2.0f * t - 1.0f;
      out -= poly_blep(t, dt);
    } else if (morph >= 0.99999f) {
      const float pw = clampfLocal(pulseWidth, 0.05f, 0.95f);
      out = (t < pw) ? 1.0f : -1.0f;
      out += poly_blep(t, dt);
      float t2 = t - pw;
      if (t2 < 0.0f) t2 += 1.0f;
      out -= poly_blep(t2, dt);
    } else {
      float saw = 2.0f * t - 1.0f;
      saw -= poly_blep(t, dt);

      const float pw = clampfLocal(pulseWidth, 0.05f, 0.95f);
      float sqr = (t < pw) ? 1.0f : -1.0f;
      sqr += poly_blep(t, dt);
      float t2 = t - pw;
      if (t2 < 0.0f) t2 += 1.0f;
      sqr -= poly_blep(t2, dt);

      out = saw + (sqr - saw) * morph;
    }

    phase += phaseInc;
    if (phase >= 1.0f) phase -= 1.0f;
    return isfinite(out) ? out : 0.0f;
  }

private:
  void updatePhaseInc() {
    phaseInc = clampSafe(frequency * invSampleRate, 0.000001f, 0.45f,
                         110.0f / SAMPLE_RATE);
  }

  float sampleRate = SAMPLE_RATE;
  float invSampleRate = 1.0f / SAMPLE_RATE;
  float frequency = 110.0f;
  float phase = 0.0f;
  float phaseInc = 110.0f / SAMPLE_RATE;
  float pulseWidth = 0.5f;
  float morph = 0.0f;
  Waveform wave = WAVE_SAW;
};

class BassCascadeFilter {
public:
  static constexpr int LUT_SIZE = 257;

  void setSampleRate(float sr) {
    sampleRate = sr;
    buildCutoffLut();
    dirty = true;
  }
  void setCutoffNormalized(float c) {
    c = clampfLocal(c, 0.0f, 1.0f);
    if (fabsf(c - cutoffNorm) > 0.0015f) {
      cutoffNorm = c;
      dirty = true;
    }
  }
  void setResonance(float r) { resonance = clampfLocal(r, 0.0f, 1.0f); }
  void setDrive(float d) { drive = clampfLocal(d, 0.5f, 4.0f); }
  void reset() { y1 = y2 = y3 = y4 = 0.0f; }

  float process(float x) {
    if (dirty) updateCoeff();
    const float fb = resonance * (3.2f - 0.5f * g);
    const float u = tanhf((x - fb * y4) * drive);
    y1 += g * (u - y1);
    y2 += g * (y1 - y2);
    y3 += g * (y2 - y3);
    y4 += g * (y3 - y4);
    return 0.4f * y2 + 0.6f * y4;
  }

private:
  void buildCutoffLut() {
    constexpr float minHz = 18.0f;
    constexpr float maxHz = 9000.0f;
    for (int i = 0; i < LUT_SIZE; ++i) {
      const float n = (float)i / (float)(LUT_SIZE - 1);
      const float hz = minHz * powf(maxHz / minHz, n);
      const float x = 2.0f * PI * hz / sampleRate;
      cutoffGLut[i] = clampfLocal(x / (1.0f + x), 0.00005f, 0.82f);
    }
  }

  inline void updateCoeff() {
    const float pos = cutoffNorm * (float)(LUT_SIZE - 1);
    int i0 = (int)pos;
    if (i0 < 0) i0 = 0;
    if (i0 >= LUT_SIZE - 1) {
      g = cutoffGLut[LUT_SIZE - 1];
    } else {
      const float frac = pos - (float)i0;
      g = cutoffGLut[i0] +
          (cutoffGLut[i0 + 1] - cutoffGLut[i0]) * frac;
    }
    dirty = false;
  }

  float sampleRate = SAMPLE_RATE;
  float cutoffNorm = 0.3f;
  float resonance = 0.1f;
  float drive = 1.0f;
  float g = 0.05f;
  bool dirty = true;
  float cutoffGLut[LUT_SIZE] = {};
  float y1 = 0.0f, y2 = 0.0f, y3 = 0.0f, y4 = 0.0f;
};

class SimpleLFO {
public:
  void setSampleRate(float sr) {
    sampleRate = sr;
    invSampleRate = 1.0f / max(1.0f, sr);
    updatePhaseInc();
  }
  void setRateHz(float hz) {
    rateHz = clampSafe(hz, 0.01f, 20.0f, 1.0f);
    updatePhaseInc();
  }
  void reset(float p = 0.0f) {
    phase = p - floorf(p);
    if (phase < 0.0f) phase += 1.0f;
  }
  float processTriangle() {
    phase += phaseInc;
    if (phase >= 1.0f) phase -= 1.0f;
    return (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
  }

private:
  void updatePhaseInc() {
    phaseInc = clampSafe(rateHz * invSampleRate, 0.0000001f, 0.25f,
                         1.0f / SAMPLE_RATE);
  }
  float sampleRate = SAMPLE_RATE;
  float invSampleRate = 1.0f / SAMPLE_RATE;
  float rateHz = 1.0f;
  float phase = 0.0f;
  float phaseInc = 1.0f / SAMPLE_RATE;
};

class SimpleFlanger {
public:
  static constexpr uint32_t MAX_SAMPLES = 1024;

  void begin() {
    reset();
    lfo.setSampleRate(SAMPLE_RATE);
  }
  void reset() {
    for (uint32_t i = 0; i < MAX_SAMPLES; ++i) buf[i] = 0.0f;
    writePos = 0;
    lfo.reset();
  }
  // Additive flanger return. The dry path is mixed outside this class.
  // 0.45 max return keeps the classic comb effect strong without a large
  // level jump at VOL=100 %.
  void setVolume(float v) { wet = 0.675f * clampfLocal(v, 0.0f, 1.0f); }
  void setRateHz(float hz) { lfo.setRateHz(clampSafe(hz, 0.05f, 3.0f, 0.18f)); }
  void setDepthNorm(float d) {
    depthNorm = clampfLocal(d, 0.0f, 1.0f);
    depthDelaySamples = depthNorm * 2.20f * (SAMPLE_RATE * 0.001f);
  }
  void setFeedback(float fb) {
    feedback = 0.72f * clampfLocal(fb, 0.0f, 1.0f);
    feedbackSigned = negativeFeedback ? -feedback : feedback;
  }
  void setManualNorm(float m) {
    manualNorm = clampfLocal(m, 0.0f, 1.0f);
    baseDelaySamples =
        (0.35f + manualNorm * 2.65f) * (SAMPLE_RATE * 0.001f);
  }
  void setNegativeFeedback(bool neg) {
    negativeFeedback = neg;
    feedbackSigned = negativeFeedback ? -feedback : feedback;
  }
  void setStereoWidth(float w) {
    stereoWidth = clampfLocal(w, 0.0f, 1.0f);
    stereoSideScale = stereoWidth * 0.55f;
  }

  inline void process(float in, float &returnMono, float &returnL, float &returnR) {
    if (wet <= 0.0001f) {
      returnMono = 0.0f;
      returnL = 0.0f;
      returnR = 0.0f;
      return;
    }

    const float tri = lfo.processTriangle();
    float delaySamples = baseDelaySamples + depthDelaySamples * tri;
    delaySamples = clampSafe(delaySamples, 1.0f,
                             (float)(MAX_SAMPLES - 3), 16.0f);

    float readPos = (float)writePos - delaySamples;
    if (readPos < 0.0f) readPos += (float)MAX_SAMPLES;

    const int i0 = (int)readPos;
    int i1 = i0 + 1;
    if (i1 >= (int)MAX_SAMPLES) i1 -= MAX_SAMPLES;
    const float frac = readPos - (float)i0;

    const float delayed = buf[i0] + (buf[i1] - buf[i0]) * frac;
    buf[writePos] = fastSoftClip(in + delayed * feedbackSigned);

    if (++writePos >= MAX_SAMPLES) writePos = 0;

    const float wetMono = delayed * wet;
    returnMono = wetMono;

    if (stereoWidth <= 0.0001f) {
      returnL = wetMono;
      returnR = wetMono;
      return;
    }

    const float side = wetMono * stereoSideScale * tri;
    returnL = wetMono + side;
    returnR = wetMono - side;
  }

private:
  float buf[MAX_SAMPLES] = {};
  uint32_t writePos = 0;
  SimpleLFO lfo;
  float wet = 0.0f;
  float depthNorm = 0.3f;
  float feedback = 0.1f;
  float feedbackSigned = 0.1f;
  float manualNorm = 0.2f;
  bool negativeFeedback = false;
  float stereoWidth = 0.25f;
  float baseDelaySamples = 0.35f * (SAMPLE_RATE * 0.001f);
  float depthDelaySamples = 0.3f * 2.20f * (SAMPLE_RATE * 0.001f);
  float stereoSideScale = 0.25f * 0.55f;
};

class PingPongDelay {
public:
  static constexpr uint32_t MAX_DELAY_SAMPLES = (uint32_t)(SAMPLE_RATE * 2.0f);

  bool begin() {
    if (!bufL) bufL = (float *)heap_caps_malloc(
        sizeof(float) * MAX_DELAY_SAMPLES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bufR) bufR = (float *)heap_caps_malloc(
        sizeof(float) * MAX_DELAY_SAMPLES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bufL || !bufR) return false;
    reset();
    return true;
  }

  void reset() {
    if (bufL) memset(bufL, 0, sizeof(float) * MAX_DELAY_SAMPLES);
    if (bufR) memset(bufR, 0, sizeof(float) * MAX_DELAY_SAMPLES);
    writePos = 0;
    lpStateL = lpStateR = 0.0f;
    activeDelaySamples = pendingDelaySamples = 0.25f * SAMPLE_RATE;
    delayCrossfadeActive = false;
    delayCrossfadePos = 1.0f;
  }

  void setDelaySeconds(float sec) {
    sec = clampSafe(sec, 0.01f, 2.0f, 0.25f);
    float newSamples = sec * SAMPLE_RATE;
    newSamples = clampSafe(newSamples, 1.0f,
                           (float)(MAX_DELAY_SAMPLES - 1),
                           0.25f * SAMPLE_RATE);

    const float pendingDiff = fabsf(newSamples - pendingDelaySamples);
    pendingDelaySamples = newSamples;

    if (pendingDiff > 64.0f &&
        fabsf(pendingDelaySamples - activeDelaySamples) > 64.0f) {
      delayCrossfadeActive = true;
      delayCrossfadePos = 0.0f;
    }
  }

  void setFeedback(float fb) { feedback = clampfLocal(fb, 0.0f, 0.95f); }
  // Additive delay return. Dry is never attenuated by this control.
  void setVolume(float v) { wet = 0.65f * clampfLocal(v, 0.0f, 1.0f); }
  void setDamp(float d) {
    damp = clampfLocal(d, 0.0f, 1.0f);
    dampCoeff = 0.05f + (1.0f - damp) * 0.45f;
  }

  inline void process(float inMono, float &outL, float &outR) {
    if (!bufL || !bufR) { outL = outR = 0.0f; return; }

    uint32_t activeDelayInt = (uint32_t)(activeDelaySamples + 0.5f);
    if (activeDelayInt < 1) activeDelayInt = 1;
    if (activeDelayInt >= MAX_DELAY_SAMPLES) activeDelayInt = MAX_DELAY_SAMPLES - 1;

    const uint32_t readA = (writePos >= activeDelayInt)
        ? (writePos - activeDelayInt)
        : (writePos + MAX_DELAY_SAMPLES - activeDelayInt);

    float delayedL = bufL[readA];
    float delayedR = bufR[readA];

    if (delayCrossfadeActive) {
      uint32_t pendingInt = (uint32_t)(pendingDelaySamples + 0.5f);
      if (pendingInt < 1) pendingInt = 1;
      if (pendingInt >= MAX_DELAY_SAMPLES) pendingInt = MAX_DELAY_SAMPLES - 1;

      const uint32_t readB = (writePos >= pendingInt)
          ? (writePos - pendingInt)
          : (writePos + MAX_DELAY_SAMPLES - pendingInt);

      const float bL = bufL[readB];
      const float bR = bufR[readB];
      delayedL += (bL - delayedL) * delayCrossfadePos;
      delayedR += (bR - delayedR) * delayCrossfadePos;

      delayCrossfadePos += delayCrossfadeStep;
      if (delayCrossfadePos >= 1.0f) {
        delayCrossfadePos = 1.0f;
        delayCrossfadeActive = false;
        activeDelaySamples = pendingDelaySamples;
      }
    } else {
      activeDelaySamples = pendingDelaySamples;
    }

    lpStateL += dampCoeff * (delayedL - lpStateL);
    lpStateR += dampCoeff * (delayedR - lpStateR);

    const float writeL = fastSoftClip(inMono + lpStateR * feedback);
    const float writeR = fastSoftClip(inMono + lpStateL * feedback);

    bufL[writePos] = writeL;
    bufR[writePos] = writeR;
    if (++writePos >= MAX_DELAY_SAMPLES) writePos = 0;

    outL = delayedL * wet;
    outR = delayedR * wet;
  }

private:
  float *bufL = nullptr;
  float *bufR = nullptr;
  uint32_t writePos = 0;
  float activeDelaySamples = 0.25f * SAMPLE_RATE;
  float pendingDelaySamples = 0.25f * SAMPLE_RATE;
  bool delayCrossfadeActive = false;
  float delayCrossfadePos = 1.0f;
  float delayCrossfadeStep = 1.0f / (0.006f * SAMPLE_RATE);
  float feedback = 0.35f;
  float wet = 0.0f;
  float damp = 0.45f;
  float dampCoeff = 0.05f + (1.0f - 0.45f) * 0.45f;
  float lpStateL = 0.0f;
  float lpStateR = 0.0f;
};


// V0.14B LIGHT REVERB
// Four-delay stereo FDN. Only four PSRAM delay lines and a small Hadamard
// feedback matrix are used, deliberately keeping per-sample CPU cost low.
class LightReverb {
public:
  static constexpr uint32_t MAX_LEN = 1600;

  bool begin() {
    for (int i = 0; i < 4; ++i) {
      if (!buf[i]) {
        buf[i] = (float *)heap_caps_malloc(
            sizeof(float) * MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      }
      if (!buf[i]) return false;
    }
    reset();
    return true;
  }

  void reset() {
    for (int i = 0; i < 4; ++i) {
      if (buf[i]) memset(buf[i], 0, sizeof(float) * MAX_LEN);
      pos[i] = 0;
      lp[i] = 0.0f;
    }
  }

  // VOL is an additive reverb return level, not a dry/wet crossfade.
  // Max gain 0.25 keeps the previous output headroom while making the signal
  // topology explicit: dry/other FX stay constant and reverb is added on top.
  void setVolume(float v) { volume = clampfLocal(v, 0.0f, 1.0f) * 0.5625f; }
  void setSize(float v) {
    size = clampfLocal(v, 0.0f, 1.0f);
    // 65..100 % of the prime-ish base lengths: compact room to broad hall.
    static const uint16_t base[4] = { 1109, 1217, 1361, 1499 };
    const float scale = 0.65f + 0.35f * size;
    for (int i = 0; i < 4; ++i) {
      uint32_t n = (uint32_t)(base[i] * scale);
      if (n < 64) n = 64;
      if (n >= MAX_LEN) n = MAX_LEN - 1;
      len[i] = n;
      if (pos[i] >= len[i]) pos[i] = 0;
    }
  }
  void setDecay(float v) {
    decay = clampfLocal(v, 0.0f, 1.0f);
    feedback = 0.28f + decay * 0.58f; // 0.28 .. 0.86, stable by design
  }
  void setDamp(float v) {
    damp = clampfLocal(v, 0.0f, 1.0f);
    dampCoeff = 0.08f + (1.0f - damp) * 0.42f;
  }

  inline void process(float in, float &outL, float &outR) {
    if (volume <= 0.00001f) { outL = outR = 0.0f; return; }

    float y0 = buf[0][pos[0]];
    float y1 = buf[1][pos[1]];
    float y2 = buf[2][pos[2]];
    float y3 = buf[3][pos[3]];

    lp[0] += dampCoeff * (y0 - lp[0]);
    lp[1] += dampCoeff * (y1 - lp[1]);
    lp[2] += dampCoeff * (y2 - lp[2]);
    lp[3] += dampCoeff * (y3 - lp[3]);

    // Normalized 4x4 Hadamard feedback matrix.
    const float h0 = ( lp[0] + lp[1] + lp[2] + lp[3]) * 0.5f;
    const float h1 = ( lp[0] - lp[1] + lp[2] - lp[3]) * 0.5f;
    const float h2 = ( lp[0] + lp[1] - lp[2] - lp[3]) * 0.5f;
    const float h3 = ( lp[0] - lp[1] - lp[2] + lp[3]) * 0.5f;

    const float drive = in * 0.22f;
    buf[0][pos[0]] = fastSoftClip(drive + h1 * feedback);
    buf[1][pos[1]] = fastSoftClip(drive + h2 * feedback);
    buf[2][pos[2]] = fastSoftClip(drive + h3 * feedback);
    buf[3][pos[3]] = fastSoftClip(drive + h0 * feedback);

    for (int i = 0; i < 4; ++i) {
      if (++pos[i] >= len[i]) pos[i] = 0;
    }

    // Crossed taps provide stereo decorrelation without another delay stage.
    outL = (y0 + y2 - 0.35f * y1) * volume;
    outR = (y1 + y3 - 0.35f * y2) * volume;
  }

private:
  float *buf[4] = {nullptr, nullptr, nullptr, nullptr};
  uint32_t pos[4] = {0,0,0,0};
  uint32_t len[4] = {900,980,1100,1200};
  float lp[4] = {0,0,0,0};
  float volume = 0.0f;
  float size = 0.45f;
  float decay = 0.42f;
  float damp = 0.50f;
  float feedback = 0.52f;
  float dampCoeff = 0.29f;
};

class MiosaurusSynth {
public:
  void begin() {
    osc1.setSampleRate(SAMPLE_RATE);
    osc2.setSampleRate(SAMPLE_RATE);
    subOsc.setSampleRate(SAMPLE_RATE);
    ampEnv.setSampleRate(SAMPLE_RATE);
    filterEnv.setSampleRate(SAMPLE_RATE);
    filter.setSampleRate(SAMPLE_RATE);
    pwmLfo.setSampleRate(SAMPLE_RATE);
    pwmLfo.setRateHz(0.8f);

    osc1.setWaveform(WAVE_SAW);
    osc2.setWaveform(WAVE_SAW);
    subOsc.setWaveform(WAVE_SQUARE);

    lfoValueCurrent = 0.0f;
    lfoValueNext = pwmLfo.processTriangle();
    lfoValueStep =
        (lfoValueNext - lfoValueCurrent) / (float)CONTROL_INTERVAL;
    controlCounter = 0;
    updatePerfCache();
  }

  void noteOn(uint8_t note, uint8_t vel, bool retrigger = true) {
    const float f = clampSafe(midiNoteToFreq(note), 1.0f,
                              SAMPLE_RATE * 0.45f, 110.0f);
    const bool wasGate = gate;
    targetFreq = f;
    targetFreq2 = f * osc2Ratio;
    targetSubFreq = f * 0.5f;

    if (!wasGate) {
      currentFreq = targetFreq;
      currentFreq2 = targetFreq2;
      currentSubFreq = targetSubFreq;
      subOsc.resetPhase();
    }

    velocity = clampfLocal((float)vel / 127.0f, 0.0f, 1.0f);
    paraVel1 = velocity;
    paraVel2 = velocity;
    paraSubVel = velocity;
    activeNote = note;
    activeNote2 = note;
    keytrackNote = note;
    gate = true;
    legatoTransition = wasGate;
    paraSecondActive = false;
    paraSingleUnison = true;

    if (!wasGate || retrigger) {
      ampEnv.noteOn();
      filterEnv.noteOn();
    }
  }

  void setParaphonicNotes(int8_t note1, int8_t note2, int8_t subNote,
                          uint8_t vel1, uint8_t vel2,
                          bool retrigger, bool singleUnison) {
    if (note1 < 0) {
      allNotesOff();
      return;
    }

    const bool wasGate = gate;
    const float f1 = clampSafe(midiNoteToFreq((uint8_t)note1), 1.0f,
                               SAMPLE_RATE * 0.45f, 110.0f);
    const bool second = (note2 >= 0 && note2 != note1);
    const float f2 = second
      ? clampSafe(midiNoteToFreq((uint8_t)note2), 1.0f, SAMPLE_RATE * 0.45f, f1)
      : clampSafe(f1 * osc2Ratio, 1.0f, SAMPLE_RATE * 0.45f, f1);
    const int8_t subSrc = (subNote >= 0) ? subNote : note1;
    const float fs = clampSafe(midiNoteToFreq((uint8_t)subSrc) * 0.5f,
                               1.0f, SAMPLE_RATE * 0.45f, 55.0f);

    targetFreq = f1;
    targetFreq2 = f2;
    targetSubFreq = fs;
    legatoTransition = wasGate;

    if (!wasGate) {
      currentFreq = targetFreq;
      currentFreq2 = targetFreq2;
      currentSubFreq = targetSubFreq;
      subOsc.resetPhase();
    }

    paraVel1 = clampfLocal((float)vel1 / 127.0f, 0.0f, 1.0f);
    paraVel2 = second ? clampfLocal((float)vel2 / 127.0f, 0.0f, 1.0f) : paraVel1;
    paraSubVel = (second && subSrc == note2) ? paraVel2 : paraVel1;
    velocity = paraVel1;

    gate = true;
    activeNote = (uint8_t)note1;
    activeNote2 = second ? (uint8_t)note2 : (uint8_t)note1;
    keytrackNote = (uint8_t)subSrc;
    paraSecondActive = second;
    paraSingleUnison = (!second && singleUnison);

    if (!wasGate || retrigger) {
      ampEnv.noteOn();
      filterEnv.noteOn();
    }
  }

  void allNotesOff() {
    gate = false;
    paraSecondActive = false;
    paraSingleUnison = false;
    ampEnv.noteOff();
    filterEnv.noteOff();
  }

  void resetLfoPhase() { pwmLfo.reset(0.0f); }

  void updateParams() {
    // Pull the current V0.8 state once per audio block.
    osc1Morph = miosaurusStateGetActual(P_OSC1_WAVE);
    osc2Morph = miosaurusStateGetActual(P_OSC2_WAVE);

    osc1Lvl = miosaurusStateGetActual(P_OSC1_LEVEL);
    osc2Lvl = miosaurusStateGetActual(P_OSC2_LEVEL);
    subLvl = miosaurusStateGetActual(P_SUB_LEVEL);

    osc1Pw = miosaurusStateGetActual(P_OSC1_PW);
    osc2Pw = miosaurusStateGetActual(P_OSC2_PW);
    osc1Pwm = miosaurusStateGetActual(P_OSC1_PWM_AMT);
    osc2Pwm = miosaurusStateGetActual(P_OSC2_PWM_AMT);

    cutoff = miosaurusStateGetActual(P_FILTER_CUTOFF);
    reso = miosaurusStateGetActual(P_FILTER_RESO);
    drive = miosaurusStateGetActual(P_FILTER_DRIVE);
    filterEnvAmt = miosaurusStateGetActual(P_FILTER_ENV_AMT);
    keyTrack = miosaurusStateGetActual(P_FILTER_KEYTRACK);
    velFilterAmt = miosaurusStateGetActual(P_FILTER_VEL_AMT);

    masterVol = miosaurusStateGetActual(P_MASTER_VOL);
    glide = miosaurusStateGetActual(P_GLIDE);

    // V0.14B FIX8: stronger, musical glide curve.
    // GLIDE remains 0..1 in state/MIDI, but now maps non-linearly to
    // approximately 3 ms .. 3.0 s (99.9 % settling time).  The expensive
    // pow/exp work happens here during parameter refresh, not per audio sample.
    if (glide > 0.0001f) {
      const float g = clampfLocal(glide, 0.0f, 1.0f);
      const float glideTimeSec = 0.003f + powf(g, 2.2f) * 2.997f;
      glideCoeff = 1.0f - expf(logf(0.001f) / (glideTimeSec * SAMPLE_RATE));
      glideCoeff = clampSafe(glideCoeff, 0.000001f, 1.0f, 0.02f);
    } else {
      glideCoeff = 1.0f;
    }

    ampEnv.setAttack(miosaurusStateGetActual(P_AMP_ATTACK));
    ampEnv.setDecay(miosaurusStateGetActual(P_AMP_DECAY));
    ampEnv.setSustain(miosaurusStateGetActual(P_AMP_SUSTAIN));
    ampEnv.setRelease(miosaurusStateGetActual(P_AMP_RELEASE));

    filterEnv.setAttack(miosaurusStateGetActual(P_FILTER_ATTACK));
    filterEnv.setDecay(miosaurusStateGetActual(P_FILTER_DECAY));
    filterEnv.setSustain(miosaurusStateGetActual(P_FILTER_SUSTAIN));
    filterEnv.setRelease(miosaurusStateGetActual(P_FILTER_RELEASE));

    osc1.setMorph(osc1Morph);
    osc2.setMorph(osc2Morph);

    const float detuneSemi = miosaurusStateGetActual(P_OSC2_DETUNE);
    osc2Ratio = clampSafe(powf(2.0f, detuneSemi / 12.0f),
                          0.25f, 4.0f, 1.0f);

    const float lfoRateNorm = miosaurusStateGetActual(P_LFO_RATE);
    float lfoHz = clampSafe(0.05f * powf(240.0f, lfoRateNorm),
                            0.05f, 12.0f, 0.8f);
    if (miosaurusStateGetActual(P_LFO_SYNC) >= 0.5f &&
        miosaurusPerformanceClockSeen()) {
      int idx = (int)(miosaurusStateGetNormalized(P_LFO_DIV) * 10.999f);
      idx = constrain(idx, 0, 10);
      static const float beats[11] = {
        1.0f/6.0f, 0.25f, 0.375f, 1.0f/3.0f, 0.5f, 0.75f,
        2.0f/3.0f, 1.0f, 1.5f, 2.0f, 4.0f
      };
      float sec = 60.0f / max(20.0f, miosaurusPerformanceBpm()) * beats[idx];
      if (sec > 0.0001f) lfoHz = 1.0f / sec;
    }
    pwmLfo.setRateHz(clampSafe(lfoHz, 0.01f, 20.0f, 0.8f));
    lfoFilterAmt = miosaurusStateGetActual(P_LFO_FILTER_AMT);

    perfParaCached = miosaurusPerformancePara();
    perfVelRouteCached = miosaurusPerformanceVelocityRoute();
    perfAtRouteCached = miosaurusPerformanceAftertouchRoute();
    perfVelAmtCached = miosaurusPerformanceVelocityAmount();
    perfAtAmtCached = miosaurusPerformanceAftertouchAmount();
    modWheelCached = miosaurusPerformanceModWheelNorm();
    aftertouchTargetCached = miosaurusPerformanceAftertouchNorm();

    const float pbNorm = miosaurusPerformancePitchBendNorm();
    const float pbRange = miosaurusStateGetActual(P_PB_RANGE);
    if (fabsf(pbNorm - lastPitchBendNorm) > 0.0001f ||
        fabsf(pbRange - lastPitchBendRange) > 0.0001f) {
      lastPitchBendNorm = pbNorm;
      lastPitchBendRange = pbRange;
      pitchBendRatioCached =
          clampSafe(powf(2.0f, (pbNorm * pbRange) / 12.0f),
                    0.25f, 4.0f, 1.0f);
    }

    mixNormFullCached =
        0.68f / max(1.0f, osc1Lvl + osc2Lvl + subLvl * 0.8f);
    mixNormNoOsc2Cached =
        0.68f / max(1.0f, osc1Lvl + subLvl * 0.8f);

    // FIX3: z33-style cached velocity shaping.
    // Do NOT call powf() for every single audio sample.
    const float v = clampfLocal(velocity, 0.0f, 1.0f);
    switch (miosaurusPerformanceVelocityCurve()) {
      case MIO_VEL_SOFT:
        shapedVelocityCached = (v >= 0.9999f) ? 1.0f : powf(v, 1.15f);
        break;
      case MIO_VEL_HARD:
        shapedVelocityCached = (v >= 0.9999f) ? 1.0f : powf(v, 2.10f);
        break;
      case MIO_VEL_MID:
      default:
        shapedVelocityCached = (v >= 0.9999f) ? 1.0f : powf(v, 1.50f);
        break;
    }
    shapedVelocityCached = clampfLocal(shapedVelocityCached, 0.0f, 1.0f);
    updatePerfCache();

    if (gate) {
      targetFreq2 = targetFreq * osc2Ratio;
    }
  }

  float process() {
    updateGlide();

    const float pbRatio = pitchBendRatioCached;
    const float baseFreq2 = paraSecondActive
        ? currentFreq2
        : clampSafe(currentFreq * osc2Ratio, 1.0f, SAMPLE_RATE * 0.45f, currentFreq);
    const float baseSub = perfParaCached
        ? currentSubFreq
        : clampSafe(currentFreq * 0.5f, 1.0f, SAMPLE_RATE * 0.45f, currentFreq * 0.5f);

    osc1.setFrequency(clampSafe(currentFreq * pbRatio, 1.0f, SAMPLE_RATE * 0.45f, currentFreq));
    osc2.setFrequency(clampSafe(baseFreq2 * pbRatio, 1.0f, SAMPLE_RATE * 0.45f, baseFreq2));
    subOsc.setFrequency(clampSafe(baseSub * pbRatio, 1.0f, SAMPLE_RATE * 0.45f, baseSub));

    const float lfo = processControlLfo();

    float level2 = osc2Lvl;
    if (perfParaCached && !paraSecondActive && !paraSingleUnison) {
      level2 = 0.0f;
    }

    const float s1 = osc1.process() * osc1Lvl * paraVel1;
    const float s2 = osc2.process() * level2 * paraVel2;
    const float s3 = subOsc.process() * subLvl * 0.8f * paraSubVel;

    // FIX2: dynamic pre-filter headroom. High OSC/SUB combinations previously
    // could exceed unity before the nonlinear ladder input, so DRIVE became
    // unintended hard saturation on some presets. Keep the mix musical while
    // leaving DRIVE itself available as the deliberate distortion control.
    const float mixNorm =
        (level2 > 0.0001f) ? mixNormFullCached : mixNormNoOsc2Cached;
    const float mix = (s1 + s2 + s3) * mixNorm;

    const float amp = clampfLocal(ampEnv.process(), 0.0f, 1.0f);
    const float fenv = clampfLocal(filterEnv.process(), 0.0f, 1.0f);

    const float cutoffBase = cutoffBaseModCached;
    const float resoMod = resoModCached;
    const float driveMod = driveModCached;
    const float envAmtMod = envAmtModCached;
    const float pw1Base = pw1BaseModCached;
    const float pw2Base = pw2BaseModCached;
    float lfoAmtMod = lfoAmtModCached;

    lfoAmtMod += (lfoAmtMod >= 0.0f ? 1.0f : -1.0f) *
                 modWheelCached * 0.50f;
    lfoAmtMod = clampfLocal(lfoAmtMod, -1.0f, 1.0f);

    osc1.setPulseWidth(clampfLocal(
        pw1Base + lfo * osc1Pwm * 0.40f, 0.05f, 0.95f));
    osc2.setPulseWidth(clampfLocal(
        pw2Base + lfo * osc2Pwm * 0.40f, 0.05f, 0.95f));

    const float velFactor =
        1.0f - velFilterAmt + velFilterAmt * velocity;

    const float noteOffset = ((float)keytrackNote - 36.0f) / 48.0f;
    const float keyTrackOffset = noteOffset * keyTrack * 0.50f;

    float cutoffMod =
        cutoffBase +
        envAmtMod * fenv * velFactor +
        keyTrackOffset +
        lfo * lfoAmtMod * 0.35f;

    cutoffMod = clampfLocal(cutoffMod, 0.0f, 1.0f);

    filter.setResonance(clampfLocal(resoMod, 0.0f, 1.0f));
    filter.setDrive(clampSafe(driveMod, 0.5f, 4.0f, 1.0f));
    filter.setCutoffNormalized(cutoffMod);

    const float filtered = filter.process(mix);
    return filtered * amp * velocity * masterVol;
  }

private:
  void updateGlide() {
    if (glide <= 0.0001f || !legatoTransition) {
      currentFreq = targetFreq;
      currentFreq2 = targetFreq * osc2Ratio;
      currentSubFreq = targetFreq * 0.5f;
      return;
    }

    // FIX8: use cached time-based coefficient computed in updateParams().
    const float coeff = glideCoeff;

    const float dest2 = targetFreq * osc2Ratio;
    const float destSub = targetFreq * 0.5f;

    currentFreq += (targetFreq - currentFreq) * coeff;
    currentFreq2 += (dest2 - currentFreq2) * coeff;
    currentSubFreq += (destSub - currentSubFreq) * coeff;
  }

  float processControlLfo() {
    if (controlCounter <= 0) {
      lfoValueCurrent = lfoValueNext;
      lfoValueNext = pwmLfo.processTriangle();
      lfoValueStep =
          (lfoValueNext - lfoValueCurrent) / (float)CONTROL_INTERVAL;

      aftertouchSmoothed +=
          0.08f * (aftertouchTargetCached - aftertouchSmoothed);
      aftertouchSmoothed =
          clampfLocal(aftertouchSmoothed, 0.0f, 1.0f);
      updatePerfCache();

      controlCounter = CONTROL_INTERVAL;
    }

    const float out = lfoValueCurrent;
    lfoValueCurrent += lfoValueStep;
    --controlCounter;
    return out;
  }

  void updatePerfCache() {
    float cutoffBase = cutoff;
    float resoBase = reso;
    float driveBase = drive;
    float envAmtBase = filterEnvAmt;
    float pw1Base = osc1Pw;
    float pw2Base = osc2Pw;
    float lfoBase = lfoFilterAmt;

    const float velMod = shapedVelocityCached * perfVelAmtCached;
    switch (perfVelRouteCached) {
      case MIO_ROUTE_FILTER_ENV: envAmtBase += 0.95f * velMod; break;
      case MIO_ROUTE_CUTOFF: cutoffBase += 0.24f * velMod; break;
      case MIO_ROUTE_DRIVE: driveBase += 0.65f * velMod; break;
      case MIO_ROUTE_PW1: pw1Base += 0.12f * velMod; break;
      case MIO_ROUTE_PW2: pw2Base += 0.12f * velMod; break;
      default: break;
    }

    const float atMod = aftertouchSmoothed * perfAtAmtCached;
    switch (perfAtRouteCached) {
      case MIO_ROUTE_CUTOFF: cutoffBase += 0.35f * atMod; break;
      case MIO_ROUTE_RESO: resoBase += 0.12f * atMod; break;
      case MIO_ROUTE_LFO_FILTER:
        lfoBase += (lfoBase >= 0.0f ? 1.0f : -1.0f) *
                   0.40f * atMod;
        break;
      case MIO_ROUTE_DRIVE: driveBase += 0.45f * atMod; break;
      case MIO_ROUTE_PW1: pw1Base += 0.10f * atMod; break;
      case MIO_ROUTE_PW2: pw2Base += 0.10f * atMod; break;
      default: break;
    }

    cutoffBaseModCached = clampfLocal(cutoffBase, 0.0f, 1.0f);
    resoModCached = clampfLocal(resoBase, 0.0f, 1.0f);
    driveModCached = clampSafe(driveBase, 0.5f, 4.0f, 1.0f);
    envAmtModCached = clampSafe(envAmtBase, -1.5f, 1.5f, 0.0f);
    pw1BaseModCached = clampfLocal(pw1Base, 0.05f, 0.95f);
    pw2BaseModCached = clampfLocal(pw2Base, 0.05f, 0.95f);
    lfoAmtModCached = clampSafe(lfoBase, -1.0f, 1.0f, 0.0f);
  }

  Oscillator osc1, osc2, subOsc;
  ADSR ampEnv, filterEnv;
  BassCascadeFilter filter;
  SimpleLFO pwmLfo;

  bool gate = false;
  bool legatoTransition = false;
  uint8_t activeNote = 36;
  uint8_t keytrackNote = 36;
  float velocity = 1.0f;
  float shapedVelocityCached = 1.0f;
  float paraVel1 = 1.0f;
  float paraVel2 = 1.0f;
  float paraSubVel = 1.0f;
  uint8_t activeNote2 = 36;
  bool paraSecondActive = false;
  bool paraSingleUnison = true;
  float aftertouchSmoothed = 0.0f;

  float currentFreq = 65.406f;
  float targetFreq = 65.406f;
  float currentFreq2 = 65.406f;
  float targetFreq2 = 65.406f;
  float currentSubFreq = 32.703f;
  float targetSubFreq = 32.703f;
  float osc2Ratio = 1.0f;

  float osc1Morph = 0.0f, osc2Morph = 0.0f;
  float osc1Lvl = 0.85f, osc2Lvl = 0.80f, subLvl = 0.35f;
  float osc1Pw = 0.5f, osc2Pw = 0.5f;
  float osc1Pwm = 0.0f, osc2Pwm = 0.0f;
  float cutoff = 0.5f, reso = 0.14f, drive = 1.0f;
  float filterEnvAmt = 0.55f, keyTrack = 0.35f, velFilterAmt = 0.40f;
  float lfoFilterAmt = 0.0f;
  float masterVol = 0.70f, glide = 0.0f, glideCoeff = 1.0f;

  float pitchBendRatioCached = 1.0f;
  float lastPitchBendNorm = 999.0f;
  float lastPitchBendRange = 999.0f;

  bool perfParaCached = false;
  MioModRoute perfVelRouteCached = MIO_ROUTE_FILTER_ENV;
  MioModRoute perfAtRouteCached = MIO_ROUTE_CUTOFF;
  float perfVelAmtCached = 0.65f;
  float perfAtAmtCached = 0.50f;
  float modWheelCached = 0.0f;
  float aftertouchTargetCached = 0.0f;

  float cutoffBaseModCached = 0.5f;
  float resoModCached = 0.14f;
  float driveModCached = 1.0f;
  float envAmtModCached = 0.55f;
  float pw1BaseModCached = 0.5f;
  float pw2BaseModCached = 0.5f;
  float lfoAmtModCached = 0.0f;

  float mixNormFullCached = 0.25f;
  float mixNormNoOsc2Cached = 0.35f;

  static constexpr int CONTROL_INTERVAL = 32;
  float lfoValueCurrent = 0.0f;
  float lfoValueNext = 0.0f;
  float lfoValueStep = 0.0f;
  int controlCounter = 0;
};

static MiosaurusSynth sSynth;
static SimpleFlanger sFlanger;
static PingPongDelay sDelay;
static LightReverb sReverb;

static void consumeVoiceCommands() {
  if (!sVoiceQueue) return;
  AudioVoiceCmd cmd;
  while (xQueueReceive(sVoiceQueue, &cmd, 0) == pdTRUE) {
    switch (cmd.type) {
      case AVC_MONO:
        sSynth.noteOn((uint8_t)cmd.n1, cmd.v1, cmd.retrigger);
        break;
      case AVC_PARA:
        sSynth.setParaphonicNotes(cmd.n1, cmd.n2, cmd.sub,
                                  cmd.v1, cmd.v2,
                                  cmd.retrigger, cmd.singleUnison);
        break;
      case AVC_RELEASE:
        sSynth.allNotesOff();
        break;
      case AVC_RESET_LFO:
        sSynth.resetLfoPhase();
        break;
      case AVC_RESET_FX:
        sFlanger.reset();
        sDelay.reset();
        sReverb.reset();
        break;
    }
  }
}

static void updateFxParams() {
  sFlanger.setVolume(miosaurusStateGetActual(P_FLG_WET));
  const float flgRateN = miosaurusStateGetActual(P_FLG_RATE);
  static float lastFlgRateN = -99.0f;
  static float flgRateHzCached = 0.18f;
  if (fabsf(flgRateN - lastFlgRateN) > 0.0001f) {
    lastFlgRateN = flgRateN;
    flgRateHzCached =
        clampSafe(0.05f * powf(60.0f, flgRateN),
                  0.05f, 3.0f, 0.18f);
  }
  sFlanger.setRateHz(flgRateHzCached);
  sFlanger.setDepthNorm(miosaurusStateGetActual(P_FLG_DEPTH));
  sFlanger.setFeedback(miosaurusStateGetActual(P_FLG_FB));
  sFlanger.setManualNorm(miosaurusStateGetActual(P_FLG_MANUAL));
  sFlanger.setNegativeFeedback(
      miosaurusStateGetActual(P_FLG_POLARITY) >= 0.5f);
  sFlanger.setStereoWidth(miosaurusStateGetActual(P_FLG_WIDTH));

  sDelay.setVolume(miosaurusStateGetActual(P_DLY_WET));
  sDelay.setFeedback(miosaurusStateGetActual(P_DLY_FB));
  sDelay.setDamp(miosaurusStateGetActual(P_DLY_DAMP));

  float delaySec;
  if (miosaurusStateGetActual(P_DLY_SYNC) >= 0.5f &&
      miosaurusPerformanceClockSeen()) {
    int idx = (int)(miosaurusStateGetNormalized(P_DLY_DIV) * 10.999f);
    idx = constrain(idx, 0, 10);
    static const float beats[11] = {
      1.0f/6.0f, 0.25f, 0.375f, 1.0f/3.0f, 0.5f, 0.75f,
      2.0f/3.0f, 1.0f, 1.5f, 2.0f, 4.0f
    };
    delaySec = 60.0f / max(20.0f, miosaurusPerformanceBpm()) * beats[idx];
  } else {
    const float n = miosaurusStateGetActual(P_DLY_TIME);
    delaySec = 0.25f + clampfLocal(n, 0.0f, 1.0f) * 1.75f;
  }
  sDelay.setDelaySeconds(clampSafe(delaySec, 0.01f, 2.0f, 0.25f));

  sReverb.setVolume(miosaurusStateGetActual(P_REV_VOL));
  sReverb.setSize(miosaurusStateGetActual(P_REV_SIZE));
  sReverb.setDecay(miosaurusStateGetActual(P_REV_DECAY));
  sReverb.setDamp(miosaurusStateGetActual(P_REV_DAMP));
}

static bool initI2S() {
  i2s_chan_config_t cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cfg.dma_desc_num = 4;
  cfg.dma_frame_num = AUDIO_BLOCK_SAMPLES;

  if (i2s_new_channel(&cfg, &sTx, nullptr) != ESP_OK) return false;

  i2s_std_config_t stdcfg = {};
  stdcfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)SAMPLE_RATE);
  stdcfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);

  stdcfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  stdcfg.gpio_cfg.bclk = (gpio_num_t)PIN_BCLK;
  stdcfg.gpio_cfg.ws = (gpio_num_t)PIN_LRCK;
  stdcfg.gpio_cfg.dout = (gpio_num_t)PIN_DOUT;
  stdcfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  stdcfg.gpio_cfg.invert_flags.mclk_inv = false;
  stdcfg.gpio_cfg.invert_flags.bclk_inv = false;
  stdcfg.gpio_cfg.invert_flags.ws_inv = false;

  if (i2s_channel_init_std_mode(sTx, &stdcfg) != ESP_OK) return false;
  if (i2s_channel_enable(sTx) != ESP_OK) return false;
  return true;
}

}  // namespace

bool miosaurusAudioBegin() {
  sVoiceQueue = xQueueCreate(32, sizeof(AudioVoiceCmd));
  if (!sVoiceQueue) return false;
  sSynth.begin();
  sFlanger.begin();
  if (!sDelay.begin()) {
    
    return false;
  }
  if (!sReverb.begin()) {
    
    return false;
  }
  // Apply defaults once so delay lengths/feedback are valid before first block.
  sReverb.setSize(miosaurusStateGetActual(P_REV_SIZE));
  sReverb.setDecay(miosaurusStateGetActual(P_REV_DECAY));
  sReverb.setDamp(miosaurusStateGetActual(P_REV_DAMP));
  if (!initI2S()) {
    
    return false;
  }
  return true;
}

static void queueVoice(const AudioVoiceCmd &cmd) {
  if (!sVoiceQueue) {
    ++sDiagVoiceQueueDrops;
    return;
  }
  if (xQueueSend(sVoiceQueue, &cmd, 0) != pdTRUE) {
    ++sDiagVoiceQueueDrops;
  }
}

void miosaurusAudioSetMonoNote(uint8_t note, uint8_t velocity, bool retrigger) {
  AudioVoiceCmd c{AVC_MONO, (int8_t)note, -1, (int8_t)note,
                  velocity ? velocity : 1, velocity ? velocity : 1,
                  retrigger, true};
  queueVoice(c);
}

void miosaurusAudioSetParaNotes(int8_t note1, int8_t note2, int8_t subNote,
                                uint8_t vel1, uint8_t vel2,
                                bool retrigger, bool singleUnison) {
  AudioVoiceCmd c{AVC_PARA, note1, note2, subNote,
                  vel1 ? vel1 : 1, vel2 ? vel2 : (vel1 ? vel1 : 1),
                  retrigger, singleUnison};
  queueVoice(c);
}

void miosaurusAudioReleaseAll() {
  AudioVoiceCmd c{AVC_RELEASE,-1,-1,-1,0,0,false,false};
  queueVoice(c);
}

void miosaurusAudioResetLfo() {
  AudioVoiceCmd c{AVC_RESET_LFO,-1,-1,-1,0,0,false,false};
  queueVoice(c);
}

void miosaurusAudioResetFx() {
  AudioVoiceCmd c{AVC_RESET_FX,-1,-1,-1,0,0,false,false};
  queueVoice(c);
}

void miosaurusAudioTask(void *) {
  static int32_t output[AUDIO_BLOCK_SAMPLES * 2];

  for (;;) {
    const uint32_t renderStartUs = micros();

    consumeVoiceCommands();
    sSynth.updateParams();
    updateFxParams();

    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
      const float mono = sSynth.process();

      // V0.14B FIX2: all three effects are parallel additive returns.
      // The dry synth signal is always present at unity and is never reduced
      // when an FX VOL control is increased.
      float flgMono = 0.0f, flgL = 0.0f, flgR = 0.0f;
      sFlanger.process(mono, flgMono, flgL, flgR);

      float dlyL = 0.0f, dlyR = 0.0f;
      sDelay.process(mono, dlyL, dlyR);

      float revL = 0.0f, revR = 0.0f;
      // Reverb is fed from dry plus a small amount of the delay return so
      // echoes can sit naturally in the same room without serially coloring dry.
      const float revIn = mono + (dlyL + dlyR) * 0.18f;
      sReverb.process(revIn, revL, revR);

      float outL = fastSoftClip(mono + flgL + dlyL + revL);
      float outR = fastSoftClip(mono + flgR + dlyR + revR);

      output[i * 2 + 0] = floatToI32(outL);
      output[i * 2 + 1] = floatToI32(outR);
    }

    const uint32_t renderUs = (uint32_t)(micros() - renderStartUs);
    ++sDiagBlocks;
    sDiagRenderUsTotal += renderUs;
    if (renderUs > sDiagRenderUsMax) sDiagRenderUsMax = renderUs;
    if (renderUs >= 8000U) ++sDiagRenderOver8ms;

    size_t written = 0;
    const esp_err_t err = i2s_channel_write(
        sTx, output, sizeof(output), &written, portMAX_DELAY);

    if (err != ESP_OK) ++sDiagI2sErrors;
    if (written != sizeof(output)) ++sDiagShortWrites;
  }
}

void miosaurusAudioGetDiagnostics(MioAudioDiagnostics &out) {
  out.blocks = sDiagBlocks;
  out.renderUsTotal = sDiagRenderUsTotal;
  out.renderUsMax = sDiagRenderUsMax;
  out.renderOver8ms = sDiagRenderOver8ms;
  out.i2sErrors = sDiagI2sErrors;
  out.shortWrites = sDiagShortWrites;
  out.voiceQueueDrops = sDiagVoiceQueueDrops;
}
