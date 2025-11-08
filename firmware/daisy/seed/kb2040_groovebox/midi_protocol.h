#pragma once

// Wrapper to keep legacy include paths alive for the Daisy firmware.
// Pulls in the canonical MIDI definitions shared across both targets.
#include "../../../midi_protocol.h"

static_assert(MIDI_PROTOCOL_VERSION == 0x0001'0002,
              "Daisy firmware built with mismatched MIDI protocol");
