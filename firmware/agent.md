# Agent Instructions for kb2040-daisy-groovebox

## Project overview

This repo contains firmware for a custom groovebox built from:
- Adafruit KB2040 (UI controller, keys, encoders, OLED, boot/reset logic)
- Electrosmith Daisy Seed (synth and FX engine)

### Code layout

- KB2040 firmware (Arduino):
  - `firmware/kb2040/arduino/kb2040_groovebox_ui/kb2040_groovebox_ui.ino`
  - `firmware/kb2040/arduino/kb2040_groovebox_ui/midi_protocol.h`

- Daisy Seed firmware (Daisy + DaisySP C++):
  - `firmware/daisy/seed/kb2040_groovebox/kb2040_groovebox.cpp`
  - `firmware/daisy/seed/kb2040_groovebox/midi_protocol.h`

- Shared MIDI definitions (master copy for reference):
  - `firmware/midi_protocol.h`

The KB2040 sends MIDI over UART to the Daisy. The Daisy responds to notes, CCs, pitch bend, and sustain.

## VERY IMPORTANT RULES

1. **Single MIDI header**
   - Use **only** `midi_protocol.h` for MIDI channels and CC numbers.
   - Do NOT create new MIDI headers (no `midi_map.h`, `midi_protocol_v2.h`, etc.).
   - Keep the Arduino and Daisy copies of `midi_protocol.h` in sync with `firmware/midi_protocol.h`.

2. **Boot / DFU behavior must stay intact**
   - The A1 button on the KB2040 is used for:
     - Short press: reset Daisy
     - Long press: put Daisy into DFU mode (BOOT+RESET).
   - Do NOT remove or break this behavior in `kb2040_groovebox_ui.ino`.

3. **Structure**
   - Do not move files into new folders unless absolutely necessary.
   - `kb2040_groovebox_ui.ino` and `kb2040_groovebox.cpp` should remain the main entrypoints.

4. **When adding features**
   - Reuse the existing MIDI scheme defined in `midi_protocol.h`.
   - If you need new CCs, add them to `midi_protocol.h` and update BOTH sides (KB2040 + Daisy) to match.
   - Prefer editing existing files over creating new ones.

## Build assumptions

- KB2040 code is built and uploaded via Arduino IDE from:
  - `firmware/kb2040/arduino/kb2040_groovebox_ui/`

- Daisy code is built inside a DaisyExamples-style environment from:
  - `firmware/daisy/seed/kb2040_groovebox/` (or a mirrored copy of that folder).
