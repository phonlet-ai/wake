#include "frontend.h"
#include "internal.h"
#include "kws/runtime.hpp"
#include "acoustic.h"
#include "float_model.h"
#include "model.h"
#include <array>
namespace kws {
struct Session::Impl {
  std::shared_ptr<const ModelWeights> weights;
  std::unique_ptr<detail::Acoustic> acoustic;
  detail::Frontend frontend;
  std::unique_ptr<Detector> detector;
  detail::ResetClock reset_clock;
  std::vector<int32_t> posterior_scratch;
  detail::PcmFrames<512> input;
  enum class InputMode { unset, pcm, mel } mode = InputMode::unset;
  bool finished = false;
  Impl(std::shared_ptr<const ModelWeights> w, std::unique_ptr<Detector> d)
      : weights(std::move(w)), acoustic(weights->make_acoustic()),
        detector(std::move(d)), posterior_scratch(weights->classes()) {}
  void validate(InputMode selected, const int16_t *data, size_t count,
                size_t frames, OutputBuffers out) const {
    detail::require(!finished, "session finished");
    detail::require(mode == InputMode::unset || mode == selected,
                    "cannot mix PCM and cached mel");
    detail::require(data || !count, "null session input");
    detail::require(out.capacity >= frames, "output buffer too small");
    detail::require(detector ||
                        !(out.scores || out.raw || out.fires || out.gates),
                    "detector outputs require enrollment");
  }
  void step(const int16_t *mel, OutputBuffers out) {
    int32_t *post = out.q31 ? out.q31 : posterior_scratch.data();
    acoustic->step(mel, post);
    bool reset = reset_clock.advance(post[0]);
    if (detector)
      detector->score_frame(post, reset, out);
    if (reset) {
      frontend.reset();
      acoustic->reset();
    }
    if (detector)
      detector->fire_frame(out.fires);
    if (out.resets)
      *out.resets = reset;
  }
  OutputBuffers at(OutputBuffers out, size_t frame) const {
    return detail::frame_output(out, frame, weights->classes(),
                                detector ? detector->keywords() : 0,
                                detector ? detector->sensitivities() : 0);
  }
};
Session::Session(std::shared_ptr<const ModelWeights> weights,
                 std::unique_ptr<Detector> detector) {
  detail::require(bool(weights), "null model weights");
  detail::require(!detector || detector->classes() == weights->classes(),
                  "detector inventory width differs from model");
  impl_ = std::make_unique<Impl>(std::move(weights), std::move(detector));
}
Session::~Session() = default;
size_t Session::push_pcm(const int16_t *pcm, size_t samples,
                         OutputBuffers out) {
  size_t frames = impl_->input.available(samples);
  impl_->validate(Impl::InputMode::pcm, pcm, samples, frames, out);
  impl_->mode = Impl::InputMode::pcm;
  return impl_->input.push(pcm, samples, [&](const int16_t *frame, size_t i) {
    std::array<int16_t, AcousticContract::stacked_features> mel;
    impl_->frontend.process(frame, mel.data());
    impl_->frontend.process(frame + 256, mel.data() + 40);
    impl_->step(mel.data(), impl_->at(out, i));
  });
}
size_t Session::push_mel(const int16_t *mel, size_t frames, OutputBuffers out) {
  impl_->validate(Impl::InputMode::mel, mel, frames, frames, out);
  impl_->mode = Impl::InputMode::mel;
  for (size_t i = 0; i < frames; ++i)
    impl_->step(mel + i * 80, impl_->at(out, i));
  return frames;
}
size_t Session::finish() {
  detail::require(!impl_->finished, "session finished");
  size_t discarded = impl_->input.pending();
  impl_->input.clear();
  impl_->finished = true;
  return discarded;
}
} // namespace kws

// ---------------------------------------------------------------- batch run
// engine-multi: K whole PCM streams through one model in lockstep. Each stream
// has its own frontend, acoustic state, reset clock and detector, and every
// frame follows Session::Impl::step exactly (acoustic step, clock, detector
// score with the reset flag, frontend/acoustic reset, fire). For a float model
// the K acoustic steps share weight reads (FloatBatch); results per stream are
// bit-identical to a Session run alone.
namespace kws {
void run_batch(std::shared_ptr<const ModelWeights> weights, int k, const int16_t *const *pcm,
               const int64_t *samples, const DetectorConfig *config, int32_t *const *q31,
               int16_t *const *scores, uint8_t *const *resets) {
  detail::require(k > 0 && pcm && samples, "invalid batch");
  const int classes = weights->classes();
  std::vector<detail::Frontend> frontends(k);
  std::vector<detail::ResetClock> clocks(k);
  std::vector<std::unique_ptr<Detector>> detectors(k);
  std::vector<std::unique_ptr<detail::Acoustic>> single;
  std::unique_ptr<detail::FloatBatch> batch;
  if (weights->is_float())
    batch = std::make_unique<detail::FloatBatch>(weights->float_parameters(), k);
  else
    for (int s = 0; s < k; ++s)
      single.push_back(weights->make_acoustic());
  std::vector<int64_t> frames(k);
  int64_t longest = 0;
  for (int s = 0; s < k; ++s) {
    detail::require(samples[s] >= 0 && (pcm[s] || !samples[s]), "invalid stream");
    frames[s] = samples[s] / 512;
    longest = std::max(longest, frames[s]);
    if (config)
      detectors[s] = std::make_unique<Detector>(*config);
  }
  std::vector<std::array<int16_t, 80>> mel(k);
  std::vector<std::vector<int32_t>> scratch(k, std::vector<int32_t>(classes));
  std::vector<const int16_t *> mel_ptr(k);
  std::vector<int32_t *> post_ptr(k);
  std::vector<uint8_t> active(k);
  for (int64_t t = 0; t < longest; ++t) {
    for (int s = 0; s < k; ++s) {
      active[s] = t < frames[s];
      if (!active[s])
        continue;
      const int16_t *frame = pcm[s] + t * 512;
      frontends[s].process(frame, mel[s].data());
      frontends[s].process(frame + 256, mel[s].data() + 40);
      mel_ptr[s] = mel[s].data();
      post_ptr[s] = q31 && q31[s] ? q31[s] + t * classes : scratch[s].data();
    }
    if (batch)
      batch->step(mel_ptr.data(), post_ptr.data(), active.data());
    else
      for (int s = 0; s < k; ++s)
        if (active[s])
          single[s]->step(mel_ptr[s], post_ptr[s]);
    for (int s = 0; s < k; ++s) {
      if (!active[s])
        continue;
      const int32_t *post = post_ptr[s];
      bool reset = clocks[s].advance(post[0]);
      if (detectors[s]) {
        OutputBuffers out{1, nullptr, nullptr,
                          scores && scores[s] ? scores[s] + t * detectors[s]->keywords() : nullptr,
                          nullptr, nullptr, nullptr};
        detectors[s]->score_frame(post, reset, out);
      }
      if (reset) {
        frontends[s].reset();
        if (batch)
          batch->reset(s);
        else
          single[s]->reset();
      }
      if (detectors[s])
        detectors[s]->fire_frame(nullptr);
      if (resets && resets[s])
        resets[s][t] = reset;
    }
  }
}
} // namespace kws
