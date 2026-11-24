#include "frontend.h"
#include "internal.h"
#include "kws/runtime.hpp"
namespace kws {
struct Frontend::Impl {
  detail::Frontend kernel;
  detail::PcmFrames<256> input;
  bool finished = false;
};
Frontend::Frontend() : impl_(std::make_unique<Impl>()) {}
Frontend::~Frontend() = default;
void Frontend::reset() {
  impl_->kernel.reset();
  impl_->input.clear();
  impl_->finished = false;
}
size_t Frontend::push(const int16_t *pcm, size_t samples, int16_t *mel,
                      size_t capacity) {
  detail::require(!impl_->finished, "frontend finished");
  detail::require(pcm || !samples, "null PCM buffer");
  size_t frames = impl_->input.available(samples);
  detail::require(capacity >= frames && (mel || !frames),
                  "mel output buffer too small");
  return impl_->input.push(pcm, samples, [&](const int16_t *frame, size_t i) {
    impl_->kernel.process(frame, mel + i * 40);
  });
}
size_t Frontend::finish(Finalization policy, int16_t *mel) {
  detail::require(!impl_->finished, "frontend finished");
  bool emit = policy == Finalization::training_nearest_hop &&
              impl_->input.pending() >= 128;
  detail::require(!emit || mel, "null final mel output");
  if (emit)
    impl_->kernel.process(impl_->input.padded(), mel);
  impl_->input.clear();
  impl_->finished = true;
  return emit ? 1 : 0;
}
} // namespace kws
