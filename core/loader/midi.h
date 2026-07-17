#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/loader/wav.h"

namespace zeebulator {

// One note, already merged across all tracks/channels into a single
// absolute-tick timeline. Percussion (channel 10) isn't distinguished
// from melodic notes -- see RenderMidiToPcm.
struct MidiNote {
  uint32_t start_tick;
  uint32_t end_tick;
  int note;      // MIDI note number, 0-127 (69 = A4 = 440Hz)
  int velocity;  // 0-127
};

struct MidiFile {
  int division = 0;  // ticks per quarter note (SMPTE-timecode division is not supported)
  std::vector<MidiNote> notes;  // merged across all tracks, start_tick-ordered
  // (tick, microseconds_per_quarter_note) tempo-change points, tick-ordered,
  // always has an entry at tick 0 (defaulting to 120 BPM if the file
  // never sets one explicitly, per the SMF spec).
  std::vector<std::pair<uint32_t, uint32_t>> tempo_changes;
};

// Parses a Standard MIDI File, format 0 or 1 (format 2 -- independent
// unsynced sequences -- and SMPTE-timecode division are explicitly
// rejected rather than mis-parsed; real Double Dragon .mid files are
// format 0, confirmed -- see TASKS.md Phase 6). Returns nullopt for
// anything malformed or unsupported.
std::optional<MidiFile> ParseMidi(const uint8_t* data, size_t size);

// Renders a parsed MIDI file to PCM with a simple synthesizer: every
// note becomes a sine tone at its correct pitch and real-time position
// (ticks converted to seconds via the tempo map), amplitude scaled by
// velocity, with a short linear fade in/out to avoid clicks between
// notes. Deliberately no instrument/timbre modeling (every note sounds
// the same regardless of MIDI program-change events) and no special
// handling for the percussion channel -- a documented, honest scope for
// a first correct-but-crude synthesizer, not a full General MIDI
// implementation.
WavAudio RenderMidiToPcm(const MidiFile& midi, int sample_rate);

}  // namespace zeebulator
