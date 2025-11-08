#pragma once
#include <stdint.h>

// MIDI channels we care about right now.
// Everything currently runs on channel 1 (SYNTH).
namespace MidiCh
{
    constexpr uint8_t SYNTH = 1;   // current synth / all notes + CCs

    // Reserved for later expansion:
    constexpr uint8_t DRUM  = 2;   // (future) drum engine
    constexpr uint8_t CTRL  = 10;  // (future) control / looper / transport
}

// Controller numbers used by KB2040 + Daisy synth.
// These match your existing .ino and .cpp behavior.
namespace MidiCC
{
    // Joystick / expression
    constexpr uint8_t MODWHEEL      = 1;   // joystick Y -> mod wheel
    constexpr uint8_t VOLUME        = 7;   // master volume (param slot 7)

    // "Special" / reserved button events
    constexpr uint8_t SPECIAL       = 22;  // SELECT/START buttons send CC22

    // Sustain
    constexpr uint8_t SUSTAIN_PEDAL = 64;  // X/Y buttons -> sustain

    // Synth parameters (your 8 encoders)
    constexpr uint8_t CUTOFF        = 70;  // filter cutoff
    constexpr uint8_t RESONANCE     = 71;  // filter resonance
    constexpr uint8_t ATTACK        = 72;  // env attack
    constexpr uint8_t DECAY         = 73;  // env decay
    constexpr uint8_t SUSTAIN       = 74;  // env sustain
    constexpr uint8_t RELEASE       = 75;  // env release
    constexpr uint8_t VIBRATO_RATE  = 76;  // vibrato LFO rate
}
