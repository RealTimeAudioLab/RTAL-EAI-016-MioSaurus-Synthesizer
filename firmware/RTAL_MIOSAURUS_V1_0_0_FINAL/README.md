# RTAL MIOSAURUS V1.0.0 FINAL

Release firmware for **RTAL-EAI-016-MioSaurus-Synthesizer**.

## Highlights

- ESP32-S3 / JC4827W543 touchscreen platform
- 32 kHz audio engine, 256 frames per block
- Mono and Para2 performance modes
- OSC1/OSC2 continuous 128-step SAW↔SQR morphing
- Filter and amplifier envelopes
- LFO with FREE and MIDI-clock-synchronized operation
- Velocity and channel-aftertouch modulation routing
- Strong musical GLIDE up to approximately 3.0 s, displayed as OFF / ms / s
- Parallel additive Flanger, Ping-Pong Delay and Light Reverb returns
- 128 named SD-card presets P000–P127 with CRC32 validation and backup/recovery handling
- DIN MIDI and USB MIDI
- MIDI Learn with banks B0/B1
- CC32=0 selects B0; CC32=1 selects B1; B2 is intentionally unused
- Embedded startup splash and touch-release quarantine

## Release status

This package is the V1.0.0 FINAL source release. It is derived from the V1.0.0 RC1 candidate without functional changes to DSP, MIDI, presets, touch handling or the audio engine.

## Arduino project

The project directory and sketch file intentionally use the same name:

`RTAL_MIOSAURUS_V1_0_0_FINAL_PUBLIC/RTAL_MIOSAURUS_V1_0_0_FINAL.ino`

For the complete user manual, parameter tables, MIDI reference and GUI documentation, use the accompanying RTAL MIOSAURUS V3 PRO documentation package in the repository.
