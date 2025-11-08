#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_MCP23X17.h>
#include <Adafruit_seesaw.h>

// ------------------------- I2C ADDRESSES -----------------------------
#define OLED_ADDR    0x3C
#define MCP1_ADDR    0x26
#define MCP2_ADDR    0x27
#define GAMEPAD_ADDR 0x50
#define ENC1_ADDR    0x49
#define ENC2_ADDR    0x4A

// ------------------------- BOOT / RESET BUTTON (A1) ------------------
const int  BOOT_SW_PIN      = A1;     // your A1 momentary
const uint32_t BOOT_LONG_MS = 800;   // >800ms = long press
bool      bootPressed       = false;
bool      bootLongFired     = false;
uint32_t  bootPressStartMs  = 0;
bool      dfuFlash          = false;
bool      rstFlash          = false;

// ------------------------- Daisy RESET / BOOT lines ------------------
// KB2040 D3 -> Daisy RESET button leg (NRST)
// KB2040 D4 -> Daisy BOOT0 / BOOT button pad (through ~1k resistor)
const int DAISY_RST_PIN  = 3;
const int DAISY_BOOT_PIN = 4;

// ------------------------- OLED (SSD1309 128x64) ---------------------
U8G2_SSD1309_128X64_NONAME2_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ------------------------- MCP23X17s (keys) --------------------------
Adafruit_MCP23X17 mcp1;
Adafruit_MCP23X17 mcp2;

// 10 pins per MCP for keys
const uint8_t MCP_PINS[10] = {7,6,5,4,3, 8,9,10,11,12};

// ------------------------- seesaw Gamepad ----------------------------
Adafruit_seesaw pad;
bool            padOK = false;

const uint32_t BTN_X      = (1u << 6);
const uint32_t BTN_Y      = (1u << 2);
const uint32_t BTN_A      = (1u << 5);
const uint32_t BTN_B      = (1u << 1);
const uint32_t BTN_SELECT = (1u << 0);
const uint32_t BTN_START  = (1u << 16);
const uint32_t BUTTON_MASK = BTN_X | BTN_Y | BTN_A | BTN_B | BTN_SELECT | BTN_START;

// Joystick center calibration
int joyCenterX = 512;
int joyCenterY = 512;

// ------------------------- NeoRotary4 encoders -----------------------
Adafruit_seesaw encBoard[2];
bool            encBoardOK[2] = {false, false};

// Pushbutton pins on each quad encoder board
const uint8_t ENC_SWITCH_PINS[4] = {12, 14, 17, 9};

int32_t encPos[2][4];
bool    encPressed[2][4];

// ------------------------- Key order & labels ------------------------
// bottom row L->R: 4 3 2 1 5 15 14 13 12 11
// top row    L->R: 10 9 8 7 6 20 19 18 17 16
const uint8_t displayLabels[20] = {
  4,3,2,1,5, 15,14,13,12,11,
  10,9,8,7,6, 20,19,18,17,16
};

// ------------------------- MIDI note mapping -------------------------
// We use a clean chromatic layout: 20 consecutive semitones.
const uint8_t ROOT_NOTE = 48; // C3
uint8_t       noteForIndex[20];

int8_t  lastKeyIdx  = -1;
uint8_t lastKeyMidi = 0;

// ------------------------- MIDI helpers ------------------------------
static inline void midiSend3(uint8_t s, uint8_t d1, uint8_t d2) {
  Serial1.write(s); Serial1.write(d1); Serial1.write(d2);
}
static inline void sendNoteOn(uint8_t note, uint8_t vel)  { midiSend3(0x90, note, vel); }
static inline void sendNoteOff(uint8_t note, uint8_t vel) { midiSend3(0x80, note, vel); }
static inline void sendCC(uint8_t cc, uint8_t val)        { midiSend3(0xB0, cc, val); }

static inline void sendPitchBend(int16_t value14) {
  value14 = constrain(value14, 0, 16383);
  uint8_t lsb = value14 & 0x7F;
  uint8_t msb = (value14 >> 7) & 0x7F;
  midiSend3(0xE0, lsb, msb);
}

// Map analog joystick value to pitchbend 0..16383 around a calibrated center
int16_t pbFromAnalog(int v, int center, int dead) {
  int d = v - center;
  if (abs(d) <= dead)
    return 8192; // exact center

  long out = (long)d * 8192 / 512 + 8192;
  return (int16_t)constrain(out, 0, 16383);
}

// ------------------------- Key scan state ----------------------------
uint16_t prev1 = 0xFFFF, prev2 = 0xFFFF;

// ------------------------- Synth parameters (8 knobs) ----------------
enum ParamId {
  P_CUTOFF = 0,
  P_RESO,
  P_ATTACK,
  P_DECAY,
  P_SUSTAIN,
  P_RELEASE,
  P_VIBRATE,
  P_VOLUME,
  NUM_PARAMS
};

const uint8_t paramCC[NUM_PARAMS] = {
  70, // Cutoff
  71, // Reso
  72, // Attack
  73, // Decay
  74, // Sustain
  75, // Release
  76, // Vibrato rate
  7   // Volume
};

const char *paramShortName[NUM_PARAMS] = {
  "Cut","Res","Atk","Dec","Sus","Rel","Vrt","Vol"
};

uint8_t paramValue[NUM_PARAMS] = {
  96,  // Cutoff
  32,  // Reso
  10,  // Attack
  64,  // Decay
  100, // Sustain
  40,  // Release
  64,  // Vibrato rate
  110  // Volume
};

const uint8_t paramDefault[NUM_PARAMS] = {
  96,32,10,64,100,40,64,110
};

// ------------------------- Note name helper --------------------------
const char* NOTE_NAMES[12] = {
  "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
};

void midiNoteName(uint8_t note, char* buf, size_t len)
{
  uint8_t n   = note % 12;
  int     oct = (int)note / 12 - 1;
  snprintf(buf, len, "%s%d", NOTE_NAMES[n], oct);
}

// ------------------------- Play modes & chords -----------------------
enum PlayMode  { MODE_SINGLE = 0, MODE_CHORD = 1 };
enum ChordType { CH_MAJ = 0, CH_MIN, CH_DOM7, CH_MIN7, CH_DIM7, NUM_CHORD_TYPES };

PlayMode  g_playMode  = MODE_SINGLE;
ChordType g_chordType = CH_MAJ;

const char* modeNames[2] = {"SGL", "CHD"};
const char* chordNames[NUM_CHORD_TYPES] = {"Maj","Min","7","m7","dim7"};

const int8_t chordIntervals[NUM_CHORD_TYPES][4] = {
  {0, 4, 7, -1},   // Maj
  {0, 3, 7, -1},   // Min
  {0, 4, 7, 10},   // Dom7
  {0, 3, 7, 10},   // Min7
  {0, 3, 6,  9},   // Dim7
};
const uint8_t chordSizes[NUM_CHORD_TYPES] = {3,3,4,4,4};

// Per-key chord state so NoteOff is clean
struct KeyState {
  bool    pressed;
  uint8_t notes[4];
  uint8_t count;
};
KeyState keyState[20];

// ------------------------- Param helpers -----------------------------
void bumpParam(int p, int delta) {
  if (p < 0 || p >= NUM_PARAMS) return;
  int v = (int)paramValue[p] + delta;
  if (v < 0)   v = 0;
  if (v > 127) v = 127;
  if (v != paramValue[p]) {
    paramValue[p] = (uint8_t)v;
    sendCC(paramCC[p], paramValue[p]);
  }
}

void resetParamToDefault(int p) {
  if (p < 0 || p >= NUM_PARAMS) return;
  paramValue[p] = paramDefault[p];
  sendCC(paramCC[p], paramValue[p]);
}

// ------------------------- Daisy hardware reset ----------------------
void pulseDaisyReset()
{
  pinMode(DAISY_RST_PIN, OUTPUT);
  digitalWrite(DAISY_RST_PIN, LOW);
  delay(20);
  pinMode(DAISY_RST_PIN, INPUT);
}

// ------------------------- Daisy DFU entry (BOOT+RESET) --------------
void DaisyEnterDFU()
{
  digitalWrite(LED_BUILTIN, HIGH);

  pinMode(DAISY_BOOT_PIN, OUTPUT);
  digitalWrite(DAISY_BOOT_PIN, HIGH);
  delay(100);

  pinMode(DAISY_RST_PIN, OUTPUT);
  digitalWrite(DAISY_RST_PIN, LOW);
  delay(50);
  pinMode(DAISY_RST_PIN, INPUT);

  delay(150);
  pinMode(DAISY_BOOT_PIN, INPUT);
}

// ------------------------- Key play/release --------------------------
void playKey(uint8_t idx, uint8_t velocity)
{
  if (idx >= 20) return;
  KeyState &ks = keyState[idx];
  ks.count   = 0;
  ks.pressed = true;

  uint8_t root = noteForIndex[idx];

  if (g_playMode == MODE_SINGLE) {
    ks.notes[0] = root;
    ks.count    = 1;
    sendNoteOn(root, velocity);
  } else {
    ChordType ct = g_chordType;
    uint8_t nInt = chordSizes[ct];
    for (uint8_t i = 0; i < nInt && i < 4; i++) {
      int8_t semi = chordIntervals[ct][i];
      uint8_t n   = root + semi;
      ks.notes[i] = n;
      ks.count++;
      sendNoteOn(n, velocity);
    }
  }
}

void releaseKey(uint8_t idx)
{
  if (idx >= 20) return;
  KeyState &ks = keyState[idx];
  for (uint8_t i = 0; i < ks.count; i++) {
    sendNoteOff(ks.notes[i], 64);
  }
  ks.count   = 0;
  ks.pressed = false;
}

// ------------------------- OLED UI -----------------------------------
void drawUI(bool keyActive, uint8_t keyLabel, uint8_t midiNote)
{
  u8g2.clearBuffer();
  char line[32];

  // --- Note card at top ----------------------------------------------
  u8g2.drawRFrame(0, 0, 128, 18, 3);

  if (keyActive) {
    char noteStr[8];
    midiNoteName(midiNote, noteStr, sizeof(noteStr));

    u8g2.setFont(u8g2_font_t0_16b_tf);
    u8g2.drawStr(4, 14, noteStr);

    u8g2.setFont(u8g2_font_6x10_tf);
    snprintf(line, sizeof(line), "K%2u", keyLabel);
    u8g2.drawStr(68, 8, line);

    snprintf(line, sizeof(line), "N%3u", midiNote);
    u8g2.drawStr(68, 16, line);
  } else {
    u8g2.setFont(u8g2_font_t0_16b_tf);
    u8g2.drawStr(4, 14, "--");

    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(68, 12, "K--");
    u8g2.drawStr(68, 16, "N---");
  }

  // DFU / RST flash inside the note card, top-right
  if (dfuFlash) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(96, 2, 30, 10);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(101, 10, "DFU");
    u8g2.setDrawColor(1);
  } else if (rstFlash) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(96, 2, 30, 10);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(101, 10, "RST");
    u8g2.setDrawColor(1);
  }

  // --- Mode line ------------------------------------------------------
  u8g2.setFont(u8g2_font_6x10_tf);
  const char* mname = modeNames[(int)g_playMode];
  const char* cname = (g_playMode == MODE_CHORD)
                        ? chordNames[(int)g_chordType]
                        : "---";
  snprintf(line, sizeof(line), "Mode:%s  Ch:%s", mname, cname);
  u8g2.drawStr(2, 28, line);

  // --- Parameters grid (4 rows x 2 params) ---------------------------
  int y = 38;
  const int dy = 8;

  snprintf(line, sizeof(line), "%s:%3u  %s:%3u",
           paramShortName[P_CUTOFF],  paramValue[P_CUTOFF],
           paramShortName[P_RESO],    paramValue[P_RESO]);
  u8g2.drawStr(2, y, line);

  y += dy;
  snprintf(line, sizeof(line), "%s:%3u  %s:%3u",
           paramShortName[P_ATTACK],  paramValue[P_ATTACK],
           paramShortName[P_DECAY],   paramValue[P_DECAY]);
  u8g2.drawStr(2, y, line);

  y += dy;
  snprintf(line, sizeof(line), "%s:%3u  %s:%3u",
           paramShortName[P_SUSTAIN], paramValue[P_SUSTAIN],
           paramShortName[P_RELEASE], paramValue[P_RELEASE]);
  u8g2.drawStr(2, y, line);

  y += dy;
  snprintf(line, sizeof(line), "%s:%3u  %s:%3u",
           paramShortName[P_VIBRATE], paramValue[P_VIBRATE],
           paramShortName[P_VOLUME],  paramValue[P_VOLUME]);
  u8g2.drawStr(2, y, line);

  u8g2.sendBuffer();
}

// ------------------------- Gamepad button edge tracking --------------
bool btnPrevX     = false;
bool btnPrevY     = false;
bool btnPrevA     = false;
bool btnPrevB     = false;
bool btnPrevSEL   = false;
bool btnPrevSTART = false;

// ------------------------- setup() -----------------------------------
void setup()
{
  Serial1.setTX(0);       // GP0 TX -> Daisy D14 (USART1 RX)
  Serial1.setRX(1);       // GP1 RX (unused)
  Serial1.begin(31250);   // MIDI baud

  pinMode(BOOT_SW_PIN, INPUT_PULLUP);
  pinMode(DAISY_RST_PIN,  INPUT);
  pinMode(DAISY_BOOT_PIN, INPUT);
  pinMode(LED_BUILTIN,    OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Wire.begin();
  Wire.setClock(400000);

  // Note map: strict chromatic
  for (int i = 0; i < 20; ++i) {
    noteForIndex[i] = ROOT_NOTE + i;
    keyState[i].pressed = false;
    keyState[i].count   = 0;
  }

  u8g2.setI2CAddress(OLED_ADDR << 1);
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, "Init KB2040 POLY");
  u8g2.sendBuffer();
  delay(300);

  // MCPs (keys)
  if (mcp1.begin_I2C(MCP1_ADDR)) {
    for (uint8_t i = 0; i < 10; i++)
      mcp1.pinMode(MCP_PINS[i], INPUT_PULLUP);
  }
  if (mcp2.begin_I2C(MCP2_ADDR)) {
    for (uint8_t i = 0; i < 10; i++)
      mcp2.pinMode(MCP_PINS[i], INPUT_PULLUP);
  }

  // Gamepad
  if (pad.begin(GAMEPAD_ADDR)) {
    pad.pinModeBulk(BUTTON_MASK, INPUT_PULLUP);
    padOK = true;

    long sumX = 0;
    long sumY = 0;
    const int samples = 64;
    for (int i = 0; i < samples; ++i) {
      int rawX = 1023 - pad.analogRead(14);
      int rawY = 1023 - pad.analogRead(15);
      sumX += rawX;
      sumY += rawY;
      delay(2);
    }
    joyCenterX = (int)(sumX / samples);
    joyCenterY = (int)(sumY / samples);

    // Initialize button prev states (not pressed = false because we use "nowX" as active state)
    btnPrevX     = false;
    btnPrevY     = false;
    btnPrevA     = false;
    btnPrevB     = false;
    btnPrevSEL   = false;
    btnPrevSTART = false;
  } else {
    padOK = false;
  }

  // Encoders
  if (encBoard[0].begin(ENC1_ADDR)) {
    encBoardOK[0] = true;
    for (int i = 0; i < 4; i++) {
      encBoard[0].pinMode(ENC_SWITCH_PINS[i], INPUT_PULLUP);
      encPressed[0][i] = false;
    }
    for (int e = 0; e < 4; e++)
      encPos[0][e] = encBoard[0].getEncoderPosition(e);
  }

  if (encBoard[1].begin(ENC2_ADDR)) {
    encBoardOK[1] = true;
    for (int i = 0; i < 4; i++) {
      encBoard[1].pinMode(ENC_SWITCH_PINS[i], INPUT_PULLUP);
      encPressed[1][i] = false;
    }
    for (int e = 0; e < 4; e++)
      encPos[1][e] = encBoard[1].getEncoderPosition(e);
  }

  // Initial param CCs
  for (int p = 0; p < NUM_PARAMS; ++p)
    sendCC(paramCC[p], paramValue[p]);

  delay(200);
  pulseDaisyReset();
}

// ------------------------- loop() ------------------------------------
void loop()
{
  uint32_t nowMs = millis();

  // -------- Keys via MCPs --------
  uint16_t mask1 = 0, mask2 = 0;
  for (uint8_t i = 0; i < 10; i++) {
    bool s1 = mcp1.digitalRead(MCP_PINS[i]); // high = released
    bool s2 = mcp2.digitalRead(MCP_PINS[i]);
    if (s1) mask1 |= (1u << i);
    if (s2) mask2 |= (1u << i);
  }

  // MCP1 -> indices 0..9 (bottom row)
  for (uint8_t i = 0; i < 10; i++) {
    bool nowPressed  = ((mask1 & (1u << i)) == 0);
    bool prevPressed = ((prev1 & (1u << i)) == 0);
    if (nowPressed && !prevPressed) {
      uint8_t idx = i;
      lastKeyIdx  = idx;
      lastKeyMidi = noteForIndex[idx];
      playKey(idx, 100);
    } else if (!nowPressed && prevPressed) {
      uint8_t idx = i;
      releaseKey(idx);
    }
  }

  // MCP2 -> indices 10..19 (top row)
  for (uint8_t i = 0; i < 10; i++) {
    bool nowPressed  = ((mask2 & (1u << i)) == 0);
    bool prevPressed = ((prev2 & (1u << i)) == 0);
    if (nowPressed && !prevPressed) {
      uint8_t idx = 10 + i;
      lastKeyIdx  = idx;
      lastKeyMidi = noteForIndex[idx];
      playKey(idx, 100);
    } else if (!nowPressed && prevPressed) {
      uint8_t idx = 10 + i;
      releaseKey(idx);
    }
  }

  prev1 = mask1;
  prev2 = mask2;

  // -------- Gamepad (joystick + buttons) --------
  int      joyX  = joyCenterX;
  int      joyY  = joyCenterY;
  uint32_t bmask = BUTTON_MASK;

  if (padOK) {
    joyX  = 1023 - pad.analogRead(14);
    joyY  = 1023 - pad.analogRead(15);
    bmask = pad.digitalReadBulk(BUTTON_MASK);
  }

  // Pitch bend with calibrated center + deadzone
  static int16_t lastPB = 8192;
  int16_t pb = 8192;
  if (padOK) {
    const int deadX = 40;
    pb = pbFromAnalog(joyX, joyCenterX, deadX);
  }
  if (abs(pb - lastPB) > 32) {
    sendPitchBend(pb);
    lastPB = pb;
  }

  // Mod wheel (CC1) from Y, centered deadzone
  static uint8_t lastMod = 255;
  uint8_t mod = 0;
  if (padOK) {
    int dy = joyY - joyCenterY;
    const int deadY = 40;
    if (abs(dy) <= deadY) {
      mod = 0;
    } else {
      float norm = (float)(abs(dy) - deadY) /
                   (float)(1023 - joyCenterY - deadY);
      if (norm < 0.0f) norm = 0.0f;
      if (norm > 1.0f) norm = 1.0f;
      mod = (uint8_t)(norm * 127.0f + 0.5f);
    }
  }
  if (abs((int)mod - (int)lastMod) > 2) {
    sendCC(1, mod);
    lastMod = mod;
  }

  // Buttons active-low -> "now pressed" flags
  bool nowX     = !(bmask & BTN_X);
  bool nowYb    = !(bmask & BTN_Y);
  bool nowA     = !(bmask & BTN_A);
  bool nowB     = !(bmask & BTN_B);
  bool nowSel   = !(bmask & BTN_SELECT);
  bool nowStart = !(bmask & BTN_START);

  // Sustain ON/OFF
  if (nowX && !btnPrevX) {
    sendCC(64, 127);
  }
  if (nowYb && !btnPrevY) {
    sendCC(64, 0);
  }

  // A: toggle play mode
  if (nowA && !btnPrevA) {
    g_playMode = (g_playMode == MODE_SINGLE) ? MODE_CHORD : MODE_SINGLE;
  }

  // B: cycle chord type
  if (nowB && !btnPrevB) {
    g_chordType = (ChordType)((((int)g_chordType) + 1) % NUM_CHORD_TYPES);
  }

  // SELECT / START: still send CC22 events (reserved for future use)
  if (nowSel && !btnPrevSEL) {
    sendCC(22, 10);
  }
  if (nowStart && !btnPrevSTART) {
    sendCC(22, 100);
  }

  btnPrevX     = nowX;
  btnPrevY     = nowYb;
  btnPrevA     = nowA;
  btnPrevB     = nowB;
  btnPrevSEL   = nowSel;
  btnPrevSTART = nowStart;

  // -------- Encoders -> CC params --------
  for (int b = 0; b < 2; ++b) {
    if (!encBoardOK[b]) continue;
    for (int e = 0; e < 4; ++e) {
      int32_t newPos = encBoard[b].getEncoderPosition(e);
      int32_t delta  = newPos - encPos[b][e];
      if (delta != 0) {
        encPos[b][e] = newPos;
        int step = (delta > 0) ? 1 : -1;
        int p    = b * 4 + e; // 0..7
        bumpParam(p, step);
      }

      bool pressed = !encBoard[b].digitalRead(ENC_SWITCH_PINS[e]);
      if (pressed && !encPressed[b][e]) {
        encPressed[b][e] = true;
        resetParamToDefault(b * 4 + e);
      } else if (!pressed && encPressed[b][e]) {
        encPressed[b][e] = false;
      }
    }
  }

  // -------- A1 short/long: short = Daisy reset, long = Daisy DFU --------
  bool bootNowLow = (digitalRead(BOOT_SW_PIN) == LOW);

  if (bootNowLow && !bootPressed) {
    bootPressed      = true;
    bootLongFired    = false;
    bootPressStartMs = nowMs;
  }

  if (bootPressed && bootNowLow && !bootLongFired &&
      (nowMs - bootPressStartMs >= BOOT_LONG_MS))
  {
    dfuFlash      = true;
    bootLongFired = true;
    DaisyEnterDFU();
  }

  if (!bootNowLow && bootPressed) {
    if (!bootLongFired) {
      rstFlash = true;
      pulseDaisyReset();
    }
    bootPressed = false;
  }

  // -------- OLED --------
  bool    hasKey  = (lastKeyIdx >= 0);
  uint8_t klabel  = hasKey ? displayLabels[lastKeyIdx] : 0;
  uint8_t kmidi   = hasKey ? lastKeyMidi               : 0;
  drawUI(hasKey, klabel, kmidi);

  dfuFlash = false;
  rstFlash = false;

  delay(6);
}
