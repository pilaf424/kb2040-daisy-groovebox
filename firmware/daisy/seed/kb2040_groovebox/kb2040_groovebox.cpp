#include "daisy_seed.h"
#include "daisysp.h"
#include "daisysp/modules/reverbsc.h"

#include "midi_protocol.h"

#include <cstdlib>

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
static const int   kNumDrumVoices   = 8;     // concurrent drum hits
static const float kPitchBendRange  = 2.0f;  // +/- 2 semitones
static const float kDetuneSemi      = 0.08f; // osc2 slight detune
static const float kMaxFilterCutoff = 10000.0f;
static const float kMinFilterCutoff = 80.0f;
#ifdef PI
static const float kTwoPi          = 2.0f * PI;
#else
#ifndef PI_F
constexpr float PI_F = 3.14159265358979323846f;
#endif
static const float kTwoPi          = 2.0f * PI_F;
#endif

enum InstrumentMode
{
    MODE_POLY_SYNTH = 0,
    MODE_DRUM_KIT   = 1,
};

// Global parameters (control from KB2040 CCs)
float g_masterGain    = 0.4f;   // CC7
float g_cutoff        = 3000.0f; // Hz (CC70)
float g_resonance     = 0.25f;  // 0..1 (CC71)
float g_attack        = 0.01f;  // seconds (CC72)
float g_decay         = 0.25f;  // seconds (CC73)
float g_sustain       = 0.8f;   // 0..1 (CC74)
float g_release       = 0.4f;   // seconds (CC75)
float g_vibratoRate   = 5.0f;   // Hz (unused for drums)
float g_vibratoDepth  = 0.25f;  // semitones, scaled by mod wheel (CC1)
float g_modWheel      = 0.0f;   // 0..1
float g_pitchBendSemi = 0.0f;   // -2..+2 semitones

// FX parameters
float g_delayTimeSec   = 0.35f; // CC77
float g_delayFeedback  = 0.35f; // CC78
float g_delayMix       = 0.25f; // CC79
float g_reverbMix      = 0.25f; // CC80
float g_reverbTime     = 0.65f; // CC81
float g_bassBoost      = 0.6f;  // CC84
float g_driveAmount    = 0.15f; // CC85
float g_looperLevel    = 0.7f;  // CC92

InstrumentMode g_instrMode = MODE_POLY_SYNTH; // CC90

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

// Bass enhancement filter
Svf g_bassFilter;

// Delay / Reverb
constexpr size_t kDelayBuffer = 48000 * 2; // up to ~2 seconds @48k
DelayLine<float, kDelayBuffer> g_delayLine;
size_t                         g_delaySamples = 48000 * 0.35f;
ReverbSc                       g_reverb;

// Looper (simple mono capture of post-FX signal)
constexpr size_t kLooperMaxSeconds = 8;
constexpr size_t kLooperMaxSamples = 48000 * kLooperMaxSeconds;
float             g_looperL[kLooperMaxSamples];
float             g_looperR[kLooperMaxSamples];
size_t            g_looperWrite = 0;
size_t            g_looperLength = 0;
size_t            g_looperPlay = 0;
bool              g_looperRecording = false;
bool              g_looperPlaying   = false;

// Drum engine -----------------------------------------------------------
struct SimpleEnv
{
    float value;
    float decay;

    void Init()
    {
        value = 0.0f;
        decay = 0.999f;
    }

    void Trigger(float amplitude, float seconds, float samplerate)
    {
        value = amplitude;
        if(seconds < 0.001f)
            seconds = 0.001f;
        decay = expf(-1.0f / (seconds * samplerate));
    }

    float Process()
    {
        float out = value;
        value *= decay;
        if(value < 1.0e-5f)
            value = 0.0f;
        return out;
    }

    bool Active() const { return value > 1.0e-4f; }
};

enum DrumType
{
    DRUM_KICK = 0,
    DRUM_SNARE,
    DRUM_HAT_CLOSED,
    DRUM_HAT_OPEN,
    DRUM_TOM_LOW,
    DRUM_TOM_HIGH,
    DRUM_CLAP,
    DRUM_PERC,
};

struct DrumVoice
{
    DrumType type;
    SimpleEnv env;
    SimpleEnv noiseEnv;
    float     phase;
    float     freq;
    float     pitchScale;
    float     pitchDecay;
    float     velocity;
    bool      active;
};

DrumVoice drumVoices[kNumDrumVoices];

float g_samplerate = 48000.0f;

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

void UpdateDelayParams()
{
    size_t minDelay = (size_t)(0.02f * g_samplerate);
    size_t maxDelay = (size_t)(1.0f * g_samplerate);
    size_t target   = (size_t)(g_delayTimeSec * g_samplerate);
    if(target < minDelay)
        target = minDelay;
    if(target > maxDelay)
        target = maxDelay;
    g_delaySamples = target;
}

void UpdateReverbParams()
{
    float fb = 0.2f + 0.75f * g_reverbTime;
    if(fb > 0.95f)
        fb = 0.95f;
    g_reverb.SetFeedback(fb);
}

void StopLooper()
{
    g_looperRecording = false;
    g_looperPlaying   = false;
    g_looperWrite     = 0;
    g_looperLength    = 0;
    g_looperPlay      = 0;
}

void StartLooperRecord()
{
    g_looperRecording = true;
    g_looperPlaying   = false;
    g_looperWrite     = 0;
    g_looperLength    = 0;
}

void FinishLooperRecord()
{
    g_looperRecording = false;
    if(g_looperWrite > 0)
    {
        g_looperLength = g_looperWrite;
        g_looperPlay   = 0;
        g_looperPlaying = true;
    }
}

void ToggleLooperPlayback()
{
    if(g_looperLength == 0)
        return;
    g_looperPlaying = !g_looperPlaying;
    if(g_looperPlaying)
        g_looperPlay = 0;
}

DrumVoice* FindDrumVoice()
{
    for(int i = 0; i < kNumDrumVoices; i++)
    {
        if(!drumVoices[i].active)
            return &drumVoices[i];
    }
    return &drumVoices[0];
}

DrumType DrumTypeForNote(int note)
{
    switch(note)
    {
        case 36: return DRUM_KICK;
        case 38: return DRUM_SNARE;
        case 39: return DRUM_CLAP;
        case 41: return DRUM_TOM_LOW;
        case 43: return DRUM_TOM_LOW;
        case 45: return DRUM_TOM_HIGH;
        case 47: return DRUM_TOM_HIGH;
        case 42: return DRUM_HAT_CLOSED;
        case 44: return DRUM_HAT_CLOSED;
        case 46: return DRUM_HAT_OPEN;
        case 49: return DRUM_PERC;
        case 51: return DRUM_PERC;
        default: return DRUM_SNARE;
    }
}

void TriggerDrum(int note, float velocity)
{
    DrumVoice* v = FindDrumVoice();
    v->type      = DrumTypeForNote(note);
    v->env.Init();
    v->noiseEnv.Init();
    v->phase       = 0.0f;
    v->velocity    = velocity;
    v->pitchScale  = 1.0f;
    v->pitchDecay  = 0.0f;
    v->active      = true;

    switch(v->type)
    {
        case DRUM_KICK:
            v->freq       = 55.0f + 40.0f * velocity;
            v->pitchScale = 3.0f + 2.0f * velocity;
            v->pitchDecay = 0.9994f;
            v->env.Trigger(1.2f * velocity, 0.35f, g_samplerate);
            v->noiseEnv.Trigger(0.4f * velocity, 0.05f, g_samplerate);
            break;
        case DRUM_SNARE:
            v->freq       = 180.0f + 80.0f * velocity;
            v->pitchScale = 1.0f;
            v->pitchDecay = 1.0f;
            v->env.Trigger(0.9f * velocity, 0.25f, g_samplerate);
            v->noiseEnv.Trigger(0.8f * velocity, 0.18f, g_samplerate);
            break;
        case DRUM_HAT_CLOSED:
            v->freq       = 6000.0f;
            v->pitchScale = 1.0f;
            v->pitchDecay = 1.0f;
            v->env.Trigger(0.6f * velocity, 0.08f, g_samplerate);
            v->noiseEnv.Trigger(0.7f * velocity, 0.05f, g_samplerate);
            break;
        case DRUM_HAT_OPEN:
            v->freq       = 5500.0f;
            v->pitchScale = 1.0f;
            v->pitchDecay = 1.0f;
            v->env.Trigger(0.6f * velocity, 0.25f, g_samplerate);
            v->noiseEnv.Trigger(0.7f * velocity, 0.20f, g_samplerate);
            break;
        case DRUM_TOM_LOW:
            v->freq       = 110.0f + 30.0f * velocity;
            v->pitchScale = 1.8f;
            v->pitchDecay = 0.9996f;
            v->env.Trigger(1.0f * velocity, 0.4f, g_samplerate);
            v->noiseEnv.Trigger(0.4f * velocity, 0.12f, g_samplerate);
            break;
        case DRUM_TOM_HIGH:
            v->freq       = 180.0f + 60.0f * velocity;
            v->pitchScale = 1.6f;
            v->pitchDecay = 0.9995f;
            v->env.Trigger(0.9f * velocity, 0.3f, g_samplerate);
            v->noiseEnv.Trigger(0.4f * velocity, 0.1f, g_samplerate);
            break;
        case DRUM_CLAP:
            v->freq       = 800.0f;
            v->pitchScale = 1.0f;
            v->pitchDecay = 1.0f;
            v->env.Trigger(0.8f * velocity, 0.18f, g_samplerate);
            v->noiseEnv.Trigger(1.0f * velocity, 0.12f, g_samplerate);
            break;
        case DRUM_PERC:
        default:
            v->freq       = 430.0f;
            v->pitchScale = 1.2f;
            v->pitchDecay = 0.9996f;
            v->env.Trigger(0.7f * velocity, 0.22f, g_samplerate);
            v->noiseEnv.Trigger(0.7f * velocity, 0.18f, g_samplerate);
            break;
    }
}

float ProcessDrums()
{
    float out = 0.0f;
    for(int i = 0; i < kNumDrumVoices; i++)
    {
        DrumVoice& v = drumVoices[i];
        if(!v.active)
            continue;

        float envOut = v.env.Process();
        float noiseOut = v.noiseEnv.Process();

        if(envOut <= 0.0f && noiseOut <= 0.0f)
        {
            v.active = false;
            continue;
        }

        float tone = 0.0f;
        if(v.type == DRUM_HAT_CLOSED || v.type == DRUM_HAT_OPEN || v.type == DRUM_CLAP)
        {
            tone = 0.0f;
        }
        else
        {
            v.phase += (v.freq * v.pitchScale) / g_samplerate;
            if(v.phase >= 1.0f)
                v.phase -= 1.0f;
            tone = sinf(kTwoPi * v.phase);
            v.pitchScale *= v.pitchDecay;
            if(v.pitchScale < 1.0f)
                v.pitchScale = 1.0f;
        }

        float noise = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;

        float mix = 0.0f;
        switch(v.type)
        {
            case DRUM_KICK:
                mix = tone * envOut + 0.2f * noise * noiseOut;
                break;
            case DRUM_SNARE:
                mix = 0.35f * tone * envOut + noise * noiseOut;
                break;
            case DRUM_HAT_CLOSED:
            case DRUM_HAT_OPEN:
                mix = noise * (0.6f * envOut + 0.9f * noiseOut);
                break;
            case DRUM_TOM_LOW:
            case DRUM_TOM_HIGH:
                mix = 0.8f * tone * envOut + 0.3f * noise * noiseOut;
                break;
            case DRUM_CLAP:
                mix = noise * (0.5f * envOut + 1.1f * noiseOut);
                break;
            case DRUM_PERC:
            default:
                mix = 0.5f * tone * envOut + 0.6f * noise * noiseOut;
                break;
        }

        out += mix * v.velocity;
        v.active = v.active && (v.env.Active() || v.noiseEnv.Active());
    }
    return out;
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
    if(channel != MidiCh::SYNTH)
        return;

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

    float vel = (float)velocity / 127.0f;

    if(g_instrMode == MODE_DRUM_KIT)
    {
        TriggerDrum(note, vel);
        return;
    }

    Voice* v = AllocateVoiceForNote(note);
    if(!v)
        return;

    v->note    = note;
    v->vel     = vel;
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
    if(channel != MidiCh::SYNTH)
        return;

    if(g_instrMode == MODE_DRUM_KIT)
    {
        return;
    }
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
    if(channel != MidiCh::SYNTH)
        return;

    float n = CCNorm(val);

    switch(cc)
    {
        case MidiCC::VOLUME:
            g_masterGain = powf(n, 1.5f); // nicer taper
            break;

        case MidiCC::CUTOFF:
        {
            float t = n * n; // more resolution at low freqs
            g_cutoff = kMinFilterCutoff
                       * powf(kMaxFilterCutoff / kMinFilterCutoff, t);
            UpdateFilterParams();
        }
        break;

        case MidiCC::RESONANCE:
            g_resonance = 0.1f + 0.9f * n; // 0.1..1.0
            UpdateFilterParams();
            break;

        case MidiCC::ATTACK:
            g_attack = 0.001f + 2.0f * n; // 1ms..2s
            UpdateEnvParams();
            break;

        case MidiCC::DECAY:
            g_decay = 0.01f + 3.0f * n; // 10ms..3s
            UpdateEnvParams();
            break;

        case MidiCC::SUSTAIN:
            g_sustain = n; // 0..1
            UpdateEnvParams();
            break;

        case MidiCC::RELEASE:
            g_release = 0.02f + 4.0f * n; // 20ms..4s
            UpdateEnvParams();
            break;

        case MidiCC::DELAY_TIME:
            g_delayTimeSec = 0.02f + 0.98f * n;
            UpdateDelayParams();
            break;

        case MidiCC::DELAY_FEEDBACK:
            g_delayFeedback = 0.02f + 0.9f * n;
            if(g_delayFeedback > 0.95f)
                g_delayFeedback = 0.95f;
            break;

        case MidiCC::DELAY_MIX:
            g_delayMix = n;
            break;

        case MidiCC::REVERB_MIX:
            g_reverbMix = n;
            break;

        case MidiCC::REVERB_TIME:
            g_reverbTime = n;
            UpdateReverbParams();
            break;

        case MidiCC::BASS_BOOST:
            g_bassBoost = n;
            break;

        case MidiCC::DRIVE:
            g_driveAmount = n;
            break;

        case MidiCC::LOOPER_LEVEL:
            g_looperLevel = n;
            break;

        case MidiCC::VIBRATO_RATE:
            g_vibratoRate = 0.1f + 8.0f * n; // 0.1..8 Hz
            g_vibrLfo.SetFreq(g_vibratoRate);
            break;

        case MidiCC::MODWHEEL:
            g_modWheel = n; // 0..1, scales vibrato depth
            break;

        case MidiCC::SUSTAIN_PEDAL:
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

        case MidiCC::INSTRUMENT_MODE:
            g_instrMode = (val >= 64) ? MODE_DRUM_KIT : MODE_POLY_SYNTH;
            break;

        case MidiCC::LOOPER_CONTROL:
            if(val < 20)
            {
                StopLooper();
            }
            else if(val < 80)
            {
                if(!g_looperRecording)
                    StartLooperRecord();
                else
                    FinishLooperRecord();
            }
            else
            {
                ToggleLooperPlayback();
            }
            break;

        default: break;
    }
}

void HandlePitchBend(uint8_t channel, uint8_t lsb, uint8_t msb)
{
    if(channel != MidiCh::SYNTH)
        return;

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

        float drum = ProcessDrums();
        if(g_instrMode == MODE_DRUM_KIT)
        {
            dry += drum;
        }

        // Global filter
        g_filter.Process(dry);
        float filtered = g_filter.Low();

        // Bass boost: add boosted low frequencies
        g_bassFilter.Process(filtered);
        float low     = g_bassFilter.Low();
        float bassMix = filtered + low * g_bassBoost;

        // Drive / saturation
        float driveGain = 1.0f + g_driveAmount * 6.0f;
        float driven    = tanhf(bassMix * driveGain);

        // Delay
        g_delayLine.SetDelay(g_delaySamples);
        float delayOut = g_delayLine.Read();
        float delayIn  = driven + delayOut * g_delayFeedback;
        g_delayLine.Write(delayIn);
        float delayMix = (1.0f - g_delayMix) * driven + g_delayMix * delayOut;

        // Reverb (stereo)
        float revL, revR;
        g_reverb.Process(delayMix, delayMix, &revL, &revR);
        float wetL = (1.0f - g_reverbMix) * delayMix + g_reverbMix * revL;
        float wetR = (1.0f - g_reverbMix) * delayMix + g_reverbMix * revR;

        // Looper record/playback on post-FX signal
        if(g_looperRecording && g_looperWrite < kLooperMaxSamples)
        {
            g_looperL[g_looperWrite] = wetL;
            g_looperR[g_looperWrite] = wetR;
            g_looperWrite++;
        }
        else if(g_looperRecording && g_looperWrite >= kLooperMaxSamples)
        {
            FinishLooperRecord();
        }

        if(g_looperPlaying && g_looperLength > 0)
        {
            wetL += g_looperL[g_looperPlay] * g_looperLevel;
            wetR += g_looperR[g_looperPlay] * g_looperLevel;
            g_looperPlay++;
            if(g_looperPlay >= g_looperLength)
                g_looperPlay = 0;
        }

        // Simple mono out to both channels
        out[0][i] = wetL * g_masterGain;
        out[1][i] = wetR * g_masterGain;
    }
}

// ----------------------------------------------------------------------
// Init
// ----------------------------------------------------------------------
void InitSynth(float samplerate)
{
    srand(0x1234);

    g_samplerate = samplerate;

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

    g_bassFilter.Init(samplerate);
    g_bassFilter.SetFreq(150.0f);
    g_bassFilter.SetRes(0.5f);

    g_delayLine.Init();
    UpdateDelayParams();

    g_reverb.Init(samplerate);
    UpdateReverbParams();

    StopLooper();

    for(int i = 0; i < kNumDrumVoices; i++)
    {
        drumVoices[i].env.Init();
        drumVoices[i].noiseEnv.Init();
        drumVoices[i].active = false;
        drumVoices[i].phase  = 0.0f;
    }

    g_masterGain   = 0.4f;
    g_sustainOn    = false;
    g_instrMode    = MODE_POLY_SYNTH;
    g_looperLevel  = 0.7f;
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
