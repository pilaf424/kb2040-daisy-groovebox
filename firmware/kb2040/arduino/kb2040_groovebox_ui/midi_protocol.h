#pragma once

// Wrapper to keep the Arduino sketch's include path stable.
// Includes the shared MIDI protocol definitions for both targets.
#include "../../../midi_protocol.h"

static_assert(MIDI_PROTOCOL_VERSION == 0x0001'0002,
              "KB2040 UI built with mismatched MIDI protocol");
