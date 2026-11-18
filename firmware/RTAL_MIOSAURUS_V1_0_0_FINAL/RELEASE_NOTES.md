# RTAL MIOSAURUS V1.0.0 FINAL PUBLIC

Public release build for `RTAL-EAI-016-MioSaurus-Synthesizer`.

## Serial / diagnostics policy

- No `Serial.begin()` in the release sketch.
- No `Serial.print`, `Serial.println` or `Serial.printf` output in the firmware sources.
- The periodic `MioValidation` task used during development is not created.
- Critical initialization failures still retain their functional `abort()` paths; they are simply silent on Serial.
- Audio, MIDI, preset, GUI and DSP behavior are otherwise unchanged from V1.0.0 FINAL PUBLIC.

This keeps the public release quiet and removes the periodic validation-task overhead used during development.
