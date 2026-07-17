#include "core/loader/midi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace zeebulator {

namespace {

uint32_t ReadU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint16_t ReadU16BE(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// Reads a MIDI variable-length quantity (7 bits per byte, MSB = "more
// bytes follow"), advancing `pos`. Returns 0 (and leaves pos unmoved
// past the end) if the buffer runs out before a terminating byte.
uint32_t ReadVlq(const uint8_t* data, size_t size, size_t& pos) {
  uint32_t value = 0;
  while (pos < size) {
    uint8_t byte = data[pos++];
    value = (value << 7) | (byte & 0x7F);
    if ((byte & 0x80) == 0) break;
  }
  return value;
}

constexpr uint32_t kDefaultMicrosecondsPerQuarter = 500000;  // 120 BPM, the SMF-spec default
constexpr double kPi = 3.14159265358979323846;

}  // namespace

std::optional<MidiFile> ParseMidi(const uint8_t* data, size_t size) {
  if (size < 14 || std::memcmp(data, "MThd", 4) != 0) return std::nullopt;
  uint32_t header_len = ReadU32BE(data + 4);
  if (header_len < 6 || 8 + header_len > size) return std::nullopt;

  uint16_t format = ReadU16BE(data + 8);
  uint16_t num_tracks = ReadU16BE(data + 10);
  uint16_t division = ReadU16BE(data + 12);
  if (format > 1) return std::nullopt;             // format 2 not supported
  if ((division & 0x8000) != 0) return std::nullopt;  // SMPTE-timecode division not supported

  MidiFile midi;
  midi.division = division;

  // (channel, note) -> (start_tick, velocity), used to pair Note On with
  // the Note Off that ends it.
  std::map<std::pair<int, int>, std::pair<uint32_t, int>> active_notes;

  size_t pos = 8 + header_len;
  for (uint16_t t = 0; t < num_tracks && pos + 8 <= size; ++t) {
    if (std::memcmp(data + pos, "MTrk", 4) != 0) return std::nullopt;
    uint32_t track_len = ReadU32BE(data + pos + 4);
    size_t track_start = pos + 8;
    size_t track_end = track_start + track_len;
    if (track_end > size) return std::nullopt;

    size_t p = track_start;
    uint32_t tick = 0;
    uint8_t running_status = 0;

    while (p < track_end) {
      tick += ReadVlq(data, track_end, p);
      if (p >= track_end) break;

      uint8_t status = data[p];
      if (status < 0x80) {
        status = running_status;  // running status: this byte is data, not a new status
      } else {
        ++p;
      }
      running_status = status;

      if (status == 0xFF) {  // meta event
        if (p >= track_end) break;
        uint8_t meta_type = data[p++];
        uint32_t len = ReadVlq(data, track_end, p);
        if (p + len > track_end) break;
        if (meta_type == 0x51 && len == 3) {  // Set Tempo
          uint32_t us_per_quarter = (static_cast<uint32_t>(data[p]) << 16) |
                                     (static_cast<uint32_t>(data[p + 1]) << 8) | data[p + 2];
          midi.tempo_changes.emplace_back(tick, us_per_quarter);
        }
        p += len;
      } else if (status == 0xF0 || status == 0xF7) {  // sysex
        uint32_t len = ReadVlq(data, track_end, p);
        if (p + len > track_end) break;
        p += len;
      } else {
        uint8_t event_type = status & 0xF0;
        int channel = status & 0x0F;
        if (event_type == 0x80 || event_type == 0x90) {  // note off / note on
          if (p + 2 > track_end) break;
          int note = data[p];
          int velocity = data[p + 1];
          p += 2;
          bool is_off = (event_type == 0x80) || (event_type == 0x90 && velocity == 0);
          auto key = std::make_pair(channel, note);
          if (is_off) {
            auto it = active_notes.find(key);
            if (it != active_notes.end()) {
              midi.notes.push_back(
                  MidiNote{it->second.first, tick, note, it->second.second});
              active_notes.erase(it);
            }
          } else {
            active_notes[key] = {tick, velocity};
          }
        } else if (event_type == 0xC0 || event_type == 0xD0) {  // 1 data byte
          if (p + 1 > track_end) break;
          p += 1;
        } else {  // 0xA0/0xB0/0xE0: 2 data bytes
          if (p + 2 > track_end) break;
          p += 2;
        }
      }
    }
    // Any notes never explicitly turned off end at the track's last tick.
    for (auto& [key, start] : active_notes) {
      midi.notes.push_back(MidiNote{start.first, tick, key.second, start.second});
    }
    active_notes.clear();

    pos = track_end;
  }

  if (midi.tempo_changes.empty() || midi.tempo_changes[0].first != 0) {
    midi.tempo_changes.insert(midi.tempo_changes.begin(), {0, kDefaultMicrosecondsPerQuarter});
  }
  std::stable_sort(midi.tempo_changes.begin(), midi.tempo_changes.end());
  std::stable_sort(midi.notes.begin(), midi.notes.end(),
                    [](const MidiNote& a, const MidiNote& b) { return a.start_tick < b.start_tick; });

  return midi;
}

namespace {

// Converts an absolute tick position to seconds, honoring every tempo
// change up to that point (a real Double Dragon .mid changes tempo mid-
// file in at least the "_l"/"_0" loop-point variants observed -- see
// TASKS.md Phase 6).
double TickToSeconds(uint32_t tick, int division,
                     const std::vector<std::pair<uint32_t, uint32_t>>& tempo_changes) {
  double seconds = 0.0;
  for (size_t i = 0; i < tempo_changes.size(); ++i) {
    uint32_t segment_start = tempo_changes[i].first;
    uint32_t segment_end = (i + 1 < tempo_changes.size()) ? tempo_changes[i + 1].first : tick;
    if (segment_start >= tick) break;
    uint32_t ticks_in_segment = std::min(segment_end, tick) - segment_start;
    double seconds_per_tick = (tempo_changes[i].second / 1'000'000.0) / division;
    seconds += ticks_in_segment * seconds_per_tick;
    if (segment_end >= tick) break;
  }
  return seconds;
}

float NoteFrequency(int note) { return 440.0f * std::pow(2.0f, (note - 69) / 12.0f); }

}  // namespace

WavAudio RenderMidiToPcm(const MidiFile& midi, int sample_rate) {
  WavAudio out;
  out.sample_rate = sample_rate;
  out.channels = 1;

  if (midi.notes.empty() || midi.division <= 0) {
    out.samples.assign(1, 0);  // avoid a zero-length clip
    return out;
  }

  uint32_t last_tick = 0;
  for (const MidiNote& n : midi.notes) last_tick = std::max(last_tick, n.end_tick);
  double total_seconds = TickToSeconds(last_tick, midi.division, midi.tempo_changes);
  size_t total_samples = static_cast<size_t>(total_seconds * sample_rate) + 1;
  std::vector<float> mix(total_samples, 0.0f);

  constexpr double kFadeSeconds = 0.01;  // linear fade in/out to avoid clicks between notes
  for (const MidiNote& n : midi.notes) {
    double start_s = TickToSeconds(n.start_tick, midi.division, midi.tempo_changes);
    double end_s = TickToSeconds(n.end_tick, midi.division, midi.tempo_changes);
    if (end_s <= start_s) continue;
    float freq = NoteFrequency(n.note);
    float amplitude = std::clamp(n.velocity / 127.0f, 0.0f, 1.0f) * 0.2f;  // headroom for polyphony
    double duration = end_s - start_s;
    double fade = std::min(kFadeSeconds, duration / 2.0);

    auto start_sample = static_cast<size_t>(start_s * sample_rate);
    auto end_sample = static_cast<size_t>(end_s * sample_rate);
    for (size_t i = start_sample; i < end_sample && i < mix.size(); ++i) {
      double t = (i - start_sample) / static_cast<double>(sample_rate);
      double env = 1.0;
      if (t < fade) {
        env = t / fade;
      } else if (duration - t < fade) {
        env = (duration - t) / fade;
      }
      mix[i] += amplitude * static_cast<float>(env) * std::sin(2.0 * kPi * freq * t);
    }
  }

  out.samples.resize(mix.size());
  for (size_t i = 0; i < mix.size(); ++i) {
    float v = std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f;
    out.samples[i] = static_cast<int16_t>(v);
  }
  return out;
}

}  // namespace zeebulator
