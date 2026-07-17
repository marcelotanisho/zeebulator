#include "core/audio/mixer.h"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

using zeebulator::Backend;
using zeebulator::Mixer;
using zeebulator::PixelFormat;
using zeebulator::ZPadState;

namespace {

class RecordingBackend : public Backend {
 public:
  void PushVideoFrame(const void*, int, int, PixelFormat) override {}
  ZPadState PollInput() override { return {}; }
  void PushAudioSamples(const int16_t* interleaved_stereo, size_t frame_count,
                         int sample_rate) override {
    ++push_count;
    last_sample_rate = sample_rate;
    last_frames.assign(interleaved_stereo, interleaved_stereo + frame_count * 2);
  }

  int push_count = 0;
  int last_sample_rate = 0;
  std::vector<int16_t> last_frames;
};

auto MakeMono(std::vector<int16_t> samples) {
  return std::make_shared<const std::vector<int16_t>>(std::move(samples));
}

}  // namespace

TEST(Mixer, SingleMonoVoiceDuplicatedToBothChannels) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({100, 200, 300}), /*channels=*/1, /*sample_rate=*/22050, /*loop=*/false);

  mixer.Mix(backend, 3);

  ASSERT_EQ(backend.push_count, 1);
  EXPECT_EQ(backend.last_sample_rate, 22050);
  std::vector<int16_t> expected = {100, 100, 200, 200, 300, 300};
  EXPECT_EQ(backend.last_frames, expected);
}

TEST(Mixer, StereoVoicePassesThroughUnmodified) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({10, 20, 30, 40}), /*channels=*/2, 22050, false);

  mixer.Mix(backend, 2);

  std::vector<int16_t> expected = {10, 20, 30, 40};
  EXPECT_EQ(backend.last_frames, expected);
}

TEST(Mixer, TwoSimultaneousVoicesSum) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({100, 100}), 1, 22050, false);
  mixer.Play(MakeMono({50, -50}), 1, 22050, false);

  mixer.Mix(backend, 2);

  std::vector<int16_t> expected = {150, 150, 50, 50};
  EXPECT_EQ(backend.last_frames, expected);
}

TEST(Mixer, OverlappingLoudVoicesClampInsteadOfWrapping) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Play(MakeMono({30000}), 1, 22050, false);
  mixer.Play(MakeMono({30000}), 1, 22050, false);  // sum = 60000, overflows int16

  mixer.Mix(backend, 1);

  EXPECT_EQ(backend.last_frames[0], 32767) << "must clamp, not wrap to negative";
  EXPECT_EQ(backend.last_frames[1], 32767);
}

TEST(Mixer, LoopingVoiceWrapsAndKeepsPlaying) {
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id = mixer.Play(MakeMono({1, 2}), 1, 22050, /*loop=*/true);

  // 5 frames from a 2-frame looping clip: 1,2,1,2,1
  mixer.Mix(backend, 5);
  std::vector<int16_t> expected = {1, 1, 2, 2, 1, 1, 2, 2, 1, 1};
  EXPECT_EQ(backend.last_frames, expected);
  EXPECT_TRUE(mixer.IsPlaying(id)) << "looping voices never finish on their own";
}

TEST(Mixer, NonLoopingVoiceFinishesAndStopsContributing) {
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id = mixer.Play(MakeMono({7, 8}), 1, 22050, /*loop=*/false);

  mixer.Mix(backend, 2);  // exactly consumes the clip
  EXPECT_FALSE(mixer.IsPlaying(id));

  mixer.Mix(backend, 2);  // nothing left to contribute
  std::vector<int16_t> expected_silence = {0, 0, 0, 0};
  EXPECT_EQ(backend.last_frames, expected_silence);
}

TEST(Mixer, PauseStopsAdvancingThenResumeContinuesFromSamePosition) {
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id = mixer.Play(MakeMono({10, 20, 30, 40}), 1, 22050, false);

  mixer.Mix(backend, 1);  // consumes sample 0 (value 10)
  mixer.Pause(id);
  mixer.Mix(backend, 1);  // paused: silence, position must not advance
  EXPECT_EQ(backend.last_frames[0], 0);

  mixer.Resume(id);
  mixer.Mix(backend, 1);  // should now emit sample index 1 (value 20), not 2
  EXPECT_EQ(backend.last_frames[0], 20);
}

TEST(Mixer, StopRemovesVoiceEntirely) {
  Mixer mixer(22050);
  RecordingBackend backend;
  Mixer::VoiceId id = mixer.Play(MakeMono({100, 100}), 1, 22050, false);

  mixer.Stop(id);
  EXPECT_FALSE(mixer.IsPlaying(id));

  mixer.Mix(backend, 1);
  EXPECT_EQ(backend.last_frames[0], 0) << "stopped voice contributes nothing";
}

TEST(Mixer, MixWithNoActiveVoicesPushesSilence) {
  Mixer mixer(22050);
  RecordingBackend backend;
  mixer.Mix(backend, 4);
  ASSERT_EQ(backend.push_count, 1);
  std::vector<int16_t> expected(8, 0);
  EXPECT_EQ(backend.last_frames, expected);
}
