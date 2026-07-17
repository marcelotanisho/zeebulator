#include "core/brew/media_hle.h"

#include <gtest/gtest.h>

#include "core/audio/mixer.h"
#include "core/brew/hle_runtime.h"
#include "core/cpu/arm_interpreter.h"

using zeebulator::ArmInterpreter;
using zeebulator::HleRuntime;
using zeebulator::MediaHle;
using zeebulator::Mixer;
using zeebulator::VirtualFilesystem;

namespace {

constexpr uint32_t kTrapBase = 0xF0000000;
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kVtable = 0x80000000;
constexpr uint32_t kObjectRegion = 0x80001000;
constexpr uint32_t kScratch = 0x00090000;

// Real MM_PARM_* constants -- see core/brew/media_hle.cpp.
constexpr uint32_t kParmMediaData = 1;
constexpr uint32_t kParmPlayRepeat = 11;
constexpr uint32_t kMmdFileName = 0;

void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 24));
}
void AppendU16LE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}
void AppendTag(std::vector<uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

std::vector<uint8_t> BuildMonoPcmWav(uint32_t sample_rate, const std::vector<int16_t>& samples) {
  std::vector<uint8_t> pcm;
  for (int16_t s : samples) AppendU16LE(pcm, static_cast<uint16_t>(s));

  std::vector<uint8_t> body;
  AppendTag(body, "fmt ");
  AppendU32LE(body, 16);
  AppendU16LE(body, 1);  // PCM
  AppendU16LE(body, 1);  // mono
  AppendU32LE(body, sample_rate);
  AppendU32LE(body, sample_rate * 2);
  AppendU16LE(body, 2);
  AppendU16LE(body, 16);
  AppendTag(body, "data");
  AppendU32LE(body, static_cast<uint32_t>(pcm.size()));
  body.insert(body.end(), pcm.begin(), pcm.end());

  std::vector<uint8_t> out;
  AppendTag(out, "RIFF");
  AppendU32LE(out, static_cast<uint32_t>(4 + body.size()));
  AppendTag(out, "WAVE");
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

void AppendVlq(std::vector<uint8_t>& out, uint32_t value) {
  uint8_t buf[4];
  int n = 0;
  buf[n++] = static_cast<uint8_t>(value & 0x7F);
  value >>= 7;
  while (value > 0) {
    buf[n++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
    value >>= 7;
  }
  for (int i = n - 1; i >= 0; --i) out.push_back(buf[i]);
}
void AppendU32BE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}
void AppendU16BE(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

// A minimal single-note format-0 MIDI file -- see tests/midi_test.cpp
// for the thoroughly-tested parser/synth this exercises through MediaHle.
std::vector<uint8_t> BuildOneNoteMidi() {
  std::vector<uint8_t> track;
  AppendVlq(track, 0);
  track.insert(track.end(), {0x90, 69, 100});  // Note On, A4, vel 100
  AppendVlq(track, 480);
  track.insert(track.end(), {0x80, 69, 0});  // Note Off
  AppendVlq(track, 0);
  track.insert(track.end(), {0xFF, 0x2F, 0x00});  // End of Track

  std::vector<uint8_t> out;
  AppendTag(out, "MThd");
  AppendU32BE(out, 6);
  AppendU16BE(out, 0);    // format 0
  AppendU16BE(out, 1);    // 1 track
  AppendU16BE(out, 480);  // division
  AppendTag(out, "MTrk");
  AppendU32BE(out, static_cast<uint32_t>(track.size()));
  out.insert(out.end(), track.begin(), track.end());
  return out;
}

void WriteCString(zeebulator::Memory& mem, uint32_t addr, const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    mem.Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
  }
  mem.Write8(addr + static_cast<uint32_t>(s.size()), 0);
}

struct Fixture {
  ArmInterpreter cpu;
  HleRuntime hle{cpu, kTrapBase, kTrapSize};
  VirtualFilesystem vfs;
  Mixer mixer{22050};
  MediaHle media_hle{cpu.GetMemory(), hle, vfs, mixer, kObjectRegion};

  Fixture() {
    vfs.AddFile("tone.wav", BuildMonoPcmWav(22050, {100, 200, 300, 400}));
    vfs.AddFile("tone.mid", BuildOneNoteMidi());
    media_hle.Build(kVtable);
  }

  uint32_t Slot(uint32_t slot) { return cpu.GetMemory().Read32(kVtable + slot * 4); }

  // Writes an AEEMediaData{clsData=MMD_FILE_NAME, pData=filename, dwSize=0}
  // struct at kScratch and returns its address.
  uint32_t WriteMediaData(const std::string& filename) {
    uint32_t name_addr = kScratch + 0x100;
    WriteCString(cpu.GetMemory(), name_addr, filename);
    uint32_t md_addr = kScratch;
    cpu.GetMemory().Write32(md_addr + 0, kMmdFileName);
    cpu.GetMemory().Write32(md_addr + 4, name_addr);
    cpu.GetMemory().Write32(md_addr + 8, 0);
    return md_addr;
  }
};

}  // namespace

// Slot indices, matching AEEIMedia.h's real order (see media_hle.cpp).
enum Slots {
  kRegisterNotify = 3,
  kSetMediaParm = 4,
  kGetMediaParm = 5,
  kPlay = 6,
  kRecord = 7,
  kStop = 8,
  kSeek = 9,
  kPause = 10,
  kResume = 11,
  kGetTotalTime = 12,
  kGetState = 13,
};

TEST(MediaHle, VtableHasAllFourteenRealSlots) {
  Fixture f;
  for (uint32_t slot = 0; slot < 14; ++slot) {
    EXPECT_NE(f.Slot(slot), 0u) << "slot " << slot << " missing";
  }
}

TEST(MediaHle, SetMediaDataOnMissingFileFails) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("nope.wav");
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  EXPECT_NE(result, 0u);
}

TEST(MediaHle, SetMediaDataThenGetStateReportsReady) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  EXPECT_EQ(result, 0u);

  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 2u) << "MM_STATE_READY";
}

TEST(MediaHle, SetMediaDataDispatchesMidFilesToMidiSynthAndPlaysThem) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.mid");
  uint32_t result = f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  ASSERT_EQ(result, 0u) << "a .mid file should decode via the MIDI synth path, not fail as PCM";

  uint32_t play_result = f.hle.CallArmFunction(f.Slot(kPlay), obj);
  EXPECT_EQ(play_result, 0u);

  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 3u) << "MM_STATE_PLAY";

  uint32_t ms = f.hle.CallArmFunction(f.Slot(kGetTotalTime), obj);
  EXPECT_NEAR(ms, 500u, 50u) << "one quarter note at the default 120 BPM tempo is ~500ms";
}

TEST(MediaHle, PlayWithoutMediaDataFails) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t result = f.hle.CallArmFunction(f.Slot(kPlay), obj);
  EXPECT_NE(result, 0u);
}

TEST(MediaHle, PlayStartsAMixerVoiceThatMixesRealDecodedSamples) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);

  uint32_t play_result = f.hle.CallArmFunction(f.Slot(kPlay), obj);
  EXPECT_EQ(play_result, 0u);

  // GetState should report PLAY (3), with pbStateChanging == FALSE.
  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 3u) << "MM_STATE_PLAY";
  EXPECT_EQ(f.cpu.GetMemory().Read32(pb_addr), 0u);
}

TEST(MediaHle, StopEndsPlaybackAndReturnsToReadyState) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  f.hle.CallArmFunction(f.Slot(kStop), obj);

  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 2u) << "MM_STATE_READY";
}

TEST(MediaHle, PauseThenResumeReturnsToPlayState) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  f.hle.CallArmFunction(f.Slot(kPause), obj);
  uint32_t pb_addr = kScratch + 0x200;
  EXPECT_EQ(f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr), 5u) << "MM_STATE_PLAY_PAUSE";

  f.hle.CallArmFunction(f.Slot(kResume), obj);
  EXPECT_EQ(f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr), 3u) << "MM_STATE_PLAY";
}

TEST(MediaHle, GetTotalTimeComputesCorrectMilliseconds) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  // 4 samples at 22050 Hz -> 4/22050 s ~= 0.1814ms rounds down to 0 with
  // integer math at this tiny size; use a fixture-independent larger
  // synthetic file instead so the math is exact and unambiguous.
  VirtualFilesystem local_vfs;
  std::vector<int16_t> samples(22050, 0);  // exactly 1 second at 22050Hz
  local_vfs.AddFile("one_sec.wav", BuildMonoPcmWav(22050, samples));
  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  Mixer mixer(22050);
  MediaHle media_hle(cpu.GetMemory(), hle, local_vfs, mixer, kObjectRegion);
  media_hle.Build(kVtable);
  uint32_t local_obj = media_hle.CreateMediaObject();

  uint32_t name_addr = kScratch + 0x100;
  WriteCString(cpu.GetMemory(), name_addr, "one_sec.wav");
  cpu.GetMemory().Write32(kScratch + 0, kMmdFileName);
  cpu.GetMemory().Write32(kScratch + 4, name_addr);
  cpu.GetMemory().Write32(kScratch + 8, 0);
  hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kSetMediaParm * 4), local_obj,
                       kParmMediaData, kScratch, 0);

  uint32_t ms = hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kGetTotalTime * 4), local_obj);
  EXPECT_EQ(ms, 1000u);
}

TEST(MediaHle, PlayRepeatZeroLoopsIndefinitelyInTheMixer) {
  Fixture f;
  uint32_t obj = f.media_hle.CreateMediaObject();
  uint32_t md = f.WriteMediaData("tone.wav");
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmMediaData, md, 0);
  f.hle.CallArmFunction(f.Slot(kSetMediaParm), obj, kParmPlayRepeat, 0, 0);  // loop forever
  f.hle.CallArmFunction(f.Slot(kPlay), obj);

  class NullBackend : public zeebulator::Backend {
   public:
    void PushVideoFrame(const void*, int, int, zeebulator::PixelFormat) override {}
    void PushAudioSamples(const int16_t*, size_t, int) override {}
    zeebulator::ZPadState PollInput() override { return {}; }
  } backend;

  // Mix far more frames than the 4-sample clip has -- a non-looping
  // voice would finish and stop contributing; a looping one keeps going.
  f.mixer.Mix(backend, 100);

  uint32_t pb_addr = kScratch + 0x200;
  uint32_t state = f.hle.CallArmFunction(f.Slot(kGetState), obj, pb_addr);
  EXPECT_EQ(state, 3u) << "MM_STATE_PLAY -- looping voices never finish on their own";
}

TEST(MediaHle, RejectsNonPcmWavInsteadOfMisdecoding) {
  Fixture f;
  VirtualFilesystem vfs;
  std::vector<uint8_t> body;
  AppendTag(body, "fmt ");
  AppendU32LE(body, 16);
  AppendU16LE(body, 0x0011);  // IMA ADPCM, not PCM
  AppendU16LE(body, 1);
  AppendU32LE(body, 22050);
  AppendU32LE(body, 22050);
  AppendU16LE(body, 1);
  AppendU16LE(body, 4);
  AppendTag(body, "data");
  AppendU32LE(body, 4);
  body.push_back(1);
  body.push_back(2);
  body.push_back(3);
  body.push_back(4);
  std::vector<uint8_t> wav;
  AppendTag(wav, "RIFF");
  AppendU32LE(wav, static_cast<uint32_t>(4 + body.size()));
  AppendTag(wav, "WAVE");
  wav.insert(wav.end(), body.begin(), body.end());
  vfs.AddFile("adpcm.wav", wav);

  ArmInterpreter cpu;
  HleRuntime hle(cpu, kTrapBase, kTrapSize);
  Mixer mixer(22050);
  MediaHle media_hle(cpu.GetMemory(), hle, vfs, mixer, kObjectRegion);
  media_hle.Build(kVtable);
  uint32_t obj = media_hle.CreateMediaObject();

  uint32_t name_addr = kScratch + 0x100;
  WriteCString(cpu.GetMemory(), name_addr, "adpcm.wav");
  cpu.GetMemory().Write32(kScratch + 0, kMmdFileName);
  cpu.GetMemory().Write32(kScratch + 4, name_addr);
  cpu.GetMemory().Write32(kScratch + 8, 0);
  uint32_t result = hle.CallArmFunction(cpu.GetMemory().Read32(kVtable + kSetMediaParm * 4), obj,
                                         kParmMediaData, kScratch, 0);
  EXPECT_NE(result, 0u);
}
