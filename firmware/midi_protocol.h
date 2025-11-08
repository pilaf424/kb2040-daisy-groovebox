// IMPORTANT:
// This is the ONE AND ONLY place for MIDI channel/CC definitions.
// Do NOT create other headers for MIDI. Update this file instead.

#pragma once
#include <stdint.h>
#include <stddef.h>

// A monotonically increasing identifier so both firmwares can assert they are
// built against the same contract.  Increment whenever the table below changes.
constexpr uint32_t MIDI_PROTOCOL_VERSION = 0x0001'0002; // v1.2

// Simple helper describing a MIDI CC entry.
struct MidiCcDefinition
{
    uint8_t       number;
    const char   *name;
    const char   *description;
};

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
    constexpr uint8_t MODWHEEL       = 1;   // joystick Y -> mod wheel
    constexpr uint8_t VOLUME         = 7;   // master volume (encoder alt)

    // Sustain
    constexpr uint8_t SUSTAIN_PEDAL  = 64;  // X/Y buttons -> sustain

    // Synth parameters (your 8 encoders)
    constexpr uint8_t CUTOFF         = 70;  // filter cutoff
    constexpr uint8_t RESONANCE      = 71;  // filter resonance
    constexpr uint8_t ATTACK         = 72;  // env attack
    constexpr uint8_t DECAY          = 73;  // env decay
    constexpr uint8_t SUSTAIN        = 74;  // env sustain
    constexpr uint8_t RELEASE        = 75;  // env release
    constexpr uint8_t VIBRATO_RATE   = 76;  // vibrato LFO rate
    constexpr uint8_t DELAY_TIME     = 77;  // delay time
    constexpr uint8_t DELAY_FEEDBACK = 78;  // delay feedback
    constexpr uint8_t DELAY_MIX      = 79;  // delay mix level
    constexpr uint8_t REVERB_MIX     = 80;  // reverb send mix
    constexpr uint8_t REVERB_TIME    = 81;  // reverb size / decay
    constexpr uint8_t BASS_BOOST     = 84;  // low boost amount
    constexpr uint8_t DRIVE          = 85;  // distortion drive
    constexpr uint8_t LOOPER_LEVEL   = 92;  // playback level for loop

    // Instrument / looper control
    constexpr uint8_t INSTRUMENT_MODE = 90; // 0=synth, >=64=drum kit
    constexpr uint8_t LOOPER_CONTROL  = 91; // values: <20 stop, ~40 record toggle, ~80 play toggle
}

// Indexed lookup tables make debugging UI events less painful and provide a
// stable ordering for documentation / telemetry dumps.
constexpr MidiCcDefinition kMidiCcTable[] = {
    {MidiCC::MODWHEEL, "Mod", "Mod wheel / joystick Y"},
    {MidiCC::VOLUME, "Vol", "Master volume"},
    {MidiCC::SUSTAIN_PEDAL, "Sus", "Sustain pedal"},
    {MidiCC::CUTOFF, "Cut", "Filter cutoff"},
    {MidiCC::RESONANCE, "Res", "Filter resonance"},
    {MidiCC::ATTACK, "Atk", "Envelope attack"},
    {MidiCC::DECAY, "Dec", "Envelope decay"},
    {MidiCC::SUSTAIN, "SusLvl", "Envelope sustain"},
    {MidiCC::RELEASE, "Rel", "Envelope release"},
    {MidiCC::VIBRATO_RATE, "Vib", "Vibrato rate"},
    {MidiCC::DELAY_TIME, "DlyT", "Delay time"},
    {MidiCC::DELAY_FEEDBACK, "DlyF", "Delay feedback"},
    {MidiCC::DELAY_MIX, "DlyM", "Delay mix"},
    {MidiCC::REVERB_MIX, "RevM", "Reverb mix"},
    {MidiCC::REVERB_TIME, "RevT", "Reverb time"},
    {MidiCC::BASS_BOOST, "Bass", "Bass boost"},
    {MidiCC::DRIVE, "Drv", "Drive"},
    {MidiCC::INSTRUMENT_MODE, "Mode", "Instrument mode"},
    {MidiCC::LOOPER_CONTROL, "LoopCtl", "Looper transport"},
    {MidiCC::LOOPER_LEVEL, "LoopLvl", "Looper playback level"},
};

constexpr size_t MidiCcCount = sizeof(kMidiCcTable) / sizeof(kMidiCcTable[0]);

inline const MidiCcDefinition *FindMidiCc(uint8_t number)
{
    for(size_t i = 0; i < MidiCcCount; ++i)
    {
        if(kMidiCcTable[i].number == number)
            return &kMidiCcTable[i];
    }
    return nullptr;
}
