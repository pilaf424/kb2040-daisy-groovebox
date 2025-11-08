#include "daisy_seed.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

// ----------------------------------------------------------------------
// Hardware
// ----------------------------------------------------------------------
DaisySeed       hw;
MidiUartHandler midi;

// ----------------------------------------------------------------------
// Synth config
// ----------------------------------------------------------------------
static const int   kNumVoices       = 6;     // polyphony
static const float kPitchBendRange  = 2.0f;  // +/- 2 semitones
static const float kDetuneSemi      = 0.08f; // osc2 slight detune
static const float kMaxFilterCutoff = 10000.0f;
static const float kMinFilterCutoff = 80.0f;

// Global parameters (control from KB2040 CCs)
float g_masterGain    = 0.4f;   // CC7
float g_cutoff        = 3000.0f; // Hz (CC70)
float g_resonance     = 0.25f;  // 0..1 (CC71)
float g_attack        = 0.01f;  // seconds (CC72)
float g_decay         = 0.25f;  // seconds (CC73)
float g_sustain       = 0.8f;   // 0..1 (CC74)
float g_release       = 0.4f;   // seconds (CC75)
float g_vibratoRate   = 5.0f;   // Hz (CC76)
float g_vibratoDepth  = 0.25f;  // semitones, scaled by mod wheel (CC1)
float g_modWheel      = 0.0f;   // 0..1
float g_pitchBendSemi = 0.0f;   // -2..+2 semitones

bool  g_sustainOn     = false;  // CC64 pedal

// ----------------------------------------------------------------------
// Voice struct
// ----------------------------------------------------------------------
struct Voice
{
    Oscillator osc1;
    Oscillator osc2;
    Adsr       env;

    int   note;      // MIDI note number
    bool  active;    // envelope still audible
    bool  gate;      // what we feed into env.Process()
    bool  keyDown;   // physical key state (from NoteOn/NoteOff)
    float vel;       // 0..1
};

Voice voices[kNumVoices];
int   voiceRotate = 0; // for voice stealing

// Global filter and vibrato LFO
Svf        g_filter;
Oscillator g_vibrLfo;

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------
float CCNorm(uint8_t v)
{
    return (float)v / 127.0f;
}

float MidiToHzWithBend(int note, float extraSemi = 0.0f)
{
    float n = (float)note + g_pitchBendSemi + extraSemi;
    return mtof(n);
}

void UpdateEnvParams()
{
    for(int i = 0; i < kNumVoices; i++)
    {
        voices[i].env.SetTime(ADSR_SEG_ATTACK,  g_attack);
        voices[i].env.SetTime(ADSR_SEG_DECAY,   g_decay);
        voices[i].env.SetTime(ADSR_SEG_RELEASE, g_release);
        voices[i].env.SetSustainLevel(g_sustain);
    }
}

void UpdateFilterParams()
{
    g_filter.SetFreq(g_cutoff);
    g_filter.SetRes(g_resonance);
}

// ----------------------------------------------------------------------
// Voice allocation with keyDown + sustain-aware gate handling
// ----------------------------------------------------------------------
Voice* FindExistingVoiceForNote(int note)
{
    for(int i = 0; i < kNumVoices; i++)
    {
        if(voices[i].note == note && (voices[i].active || voices[i].keyDown))
            return &voices[i];
    }
    return nullptr;
}

Voice* FindIdleVoice()
{
    for(int i = 0; i < kNumVoices; i++)
    {
        if(!voices[i].active && !voices[i].keyDown)
            return &voices[i];
    }
    return nullptr;
}

Voice* StealVoice()
{
    Voice* v = &voices[voiceRotate];
    voiceRotate = (voiceRotate + 1) % kNumVoices;

    v->active  = false;
    v->gate    = false;
    v->keyDown = false;
    v->vel     = 0.0f;

    return v;
}

Voice* AllocateVoiceForNote(int note)
{
    // If we already have this note, reuse that voice
    Voice* v = FindExistingVoiceForNote(note);
    if(v)
        return v;

    // Otherwise find an idle one
    v = FindIdleVoice();
    if(v)
        return v;

    // Otherwise steal one
    return StealVoice();
}

// ----------------------------------------------------------------------
// MIDI handlers
// ----------------------------------------------------------------------
void HandleNoteOn(uint8_t channel, uint8_t note, uint8_t velocity)
{
    if(velocity == 0)
    {
        // NoteOn with vel=0 is NoteOff
        for(int i = 0; i < kNumVoices; i++)
        {
            if(voices[i].note == note && voices[i].keyDown)
            {
                voices[i].keyDown = false;
                if(!g_sustainOn)
                    voices[i].gate = false;
            }
        }
        return;
    }

    Voice* v = AllocateVoiceForNote(note);
    if(!v)
        return;

    v->note    = note;
    v->vel     = (float)velocity / 127.0f;
    v->keyDown = true;
    v->gate    = true;
    v->active  = true;

    // Base pitch with bend + detune
    float baseHz  = MidiToHzWithBend(note, 0.0f);
    float detuneH = MidiToHzWithBend(note, kDetuneSemi);

    v->osc1.SetFreq(baseHz);
    v->osc2.SetFreq(detuneH);
}

void HandleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity)
{
    // Turn off *all* voices with this note whose key is down.
    for(int i = 0; i < kNumVoices; i++)
    {
        if(voices[i].note == note && voices[i].keyDown)
        {
            voices[i].keyDown = false;
            if(!g_sustainOn)
                voices[i].gate = false;
        }
    }
}

void HandleCC(uint8_t channel, uint8_t cc, uint8_t val)
{
    float n = CCNorm(val);

    switch(cc)
    {
        case 7: // Volume
            g_masterGain = powf(n, 1.5f); // nicer taper
            break;

        case 70: // Cutoff
        {
            float t = n * n; // more resolution at low freqs
            g_cutoff = kMinFilterCutoff
                       * powf(kMaxFilterCutoff / kMinFilterCutoff, t);
            UpdateFilterParams();
        }
        break;

        case 71: // Reso
            g_resonance = 0.1f + 0.9f * n; // 0.1..1.0
            UpdateFilterParams();
            break;

        case 72: // Attack
            g_attack = 0.001f + 2.0f * n; // 1ms..2s
            UpdateEnvParams();
            break;

        case 73: // Decay
            g_decay = 0.01f + 3.0f * n; // 10ms..3s
            UpdateEnvParams();
            break;

        case 74: // Sustain
            g_sustain = n; // 0..1
            UpdateEnvParams();
            break;

        case 75: // Release
            g_release = 0.02f + 4.0f * n; // 20ms..4s
            UpdateEnvParams();
            break;

        case 76: // Vibrato rate
            g_vibratoRate = 0.1f + 8.0f * n; // 0.1..8 Hz
            g_vibrLfo.SetFreq(g_vibratoRate);
            break;

        case 1: // Mod wheel from joystick Y
            g_modWheel = n; // 0..1, scales vibrato depth
            break;

        case 64: // Sustain pedal (your X/Y buttons)
        {
            bool newSustain = (val >= 64);
            if(newSustain && !g_sustainOn)
            {
                g_sustainOn = true;
            }
            else if(!newSustain && g_sustainOn)
            {
                g_sustainOn = false;
                // Pedal released: any voices with keyUp but gate still on now release
                for(int i = 0; i < kNumVoices; i++)
                {
                    if(!voices[i].keyDown && voices[i].gate)
                        voices[i].gate = false;
                }
            }
        }
        break;

        default: break;
    }
}

void HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb)
{
    // 14-bit value 0..16383, center 8192
    uint16_t value14  = ((uint16_t)msb << 7) | (uint16_t)lsb;
    int      centered = (int)value14 - 8192; // -8192..+8191

    // Deadzone around center so tiny joystick offsets don't leave
    // the synth slightly out of tune forever.
    const int dead = 256; // about 1.5% of the range
    if(centered > -dead && centered < dead)
    {
        g_pitchBendSemi = 0.0f; // snap perfectly back to in tune
        return;
    }

    float norm = (float)centered / 8192.0f; // -1..+1
    if(norm > 1.0f)
        norm = 1.0f;
    if(norm < -1.0f)
        norm = -1.0f;

    g_pitchBendSemi = norm * kPitchBendRange;
}

void ProcessMidi()
{
    midi.Listen();
    while(midi.HasEvents())
    {
        auto msg = midi.PopEvent();
        using MType = MidiMessageType;

        switch(msg.type)
        {
            case MType::NoteOn:
                HandleNoteOn(msg.channel, msg.data[0], msg.data[1]);
                break;

            case MType::NoteOff:
                HandleNoteOff(msg.channel, msg.data[0], msg.data[1]);
                break;

            case MType::ControlChange:
                HandleCC(msg.channel, msg.data[0], msg.data[1]);
                break;

            case MType::PitchBend:
                HandlePitchBend(msg.channel, msg.data[0], msg.data[1]);
                break;

            default: break;
        }
    }
}

// ----------------------------------------------------------------------
// Audio callback
// ----------------------------------------------------------------------
void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    float vibrDepth = g_vibratoDepth * g_modWheel; // semitones

    for(size_t i = 0; i < size; i++)
    {
        float dry = 0.0f;

        // Vibrato LFO (mono, -1..+1)
        float vibr = g_vibrLfo.Process();

        for(int v = 0; v < kNumVoices; v++)
        {
            Voice& voice = voices[v];

            // Skip truly idle voices
            if(!voice.active && !voice.keyDown && !voice.gate)
                continue;

            float envOut = voice.env.Process(voice.gate);

            // If envelope is fully released and there is no key or gate,
            // mark as inactive.
            if(!voice.gate && !voice.keyDown && envOut < 0.0001f)
            {
                voice.active = false;
                continue;
            }

            // Pitch with bend + vibrato
            float bendSemi = g_pitchBendSemi + (vibr * vibrDepth);
            float note     = (float)voice.note + bendSemi;
            float baseHz   = mtof(note);
            float detuneHz = mtof(note + kDetuneSemi);

            voice.osc1.SetFreq(baseHz);
            voice.osc2.SetFreq(detuneHz);

            float sig = (voice.osc1.Process() + voice.osc2.Process()) * 0.5f;
            sig *= envOut * voice.vel;

            dry += sig;
        }

        // Global filter
        g_filter.Process(dry);
        float filtered = g_filter.Low();

        // Simple mono out to both channels
        out[0][i] = filtered * g_masterGain;
        out[1][i] = filtered * g_masterGain;
    }
}

// ----------------------------------------------------------------------
// Init
// ----------------------------------------------------------------------
void InitSynth(float samplerate)
{
    for(int i = 0; i < kNumVoices; i++)
    {
        voices[i].osc1.Init(samplerate);
        voices[i].osc1.SetWaveform(Oscillator::WAVE_SAW);
        voices[i].osc1.SetAmp(0.6f);

        voices[i].osc2.Init(samplerate);
        voices[i].osc2.SetWaveform(Oscillator::WAVE_TRI);
        voices[i].osc2.SetAmp(0.6f);

        voices[i].env.Init(samplerate);
        voices[i].env.SetTime(ADSR_SEG_ATTACK,  g_attack);
        voices[i].env.SetTime(ADSR_SEG_DECAY,   g_decay);
        voices[i].env.SetTime(ADSR_SEG_RELEASE, g_release);
        voices[i].env.SetSustainLevel(g_sustain);

        voices[i].note    = 60;
        voices[i].active  = false;
        voices[i].gate    = false;
        voices[i].keyDown = false;
        voices[i].vel     = 0.0f;
    }

    g_filter.Init(samplerate);
    g_filter.SetDrive(0.0f);
    UpdateFilterParams();

    g_vibrLfo.Init(samplerate);
    g_vibrLfo.SetWaveform(Oscillator::WAVE_SIN);
    g_vibrLfo.SetFreq(g_vibratoRate);
    g_vibrLfo.SetAmp(1.0f);

    g_masterGain = 0.4f;
    g_sustainOn  = false;
}

// ----------------------------------------------------------------------
// main
// ----------------------------------------------------------------------
int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48);
    float samplerate = hw.AudioSampleRate();

    InitSynth(samplerate);

    // MIDI UART configuration: use default USART1 (Daisy Seed DIN pins).
    // You wired KB2040 TX to Daisy D14 (USART1 RX), which matches this.
    MidiUartHandler::Config midi_config;
    midi.Init(midi_config);
    midi.StartReceive();

    hw.StartAudio(AudioCallback);

    while(1)
    {
        ProcessMidi();
    }
}
