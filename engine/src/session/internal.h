#pragma once
#include "kws/runtime.hpp"
#include "scorer.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace kws::detail {
inline void require(bool condition, const char *message) {
  if (!condition)
    throw std::invalid_argument(message);
}
inline size_t checked_count(int64_t value, size_t stride = 1) {
  require(value >= 0 && uint64_t(value) <= size_t(PTRDIFF_MAX) / stride,
          "invalid buffer length");
  return size_t(value);
}
struct ResetClock {
  int high_blank = 0;
  bool advance(int32_t blank) noexcept {
    high_blank = blank >= 0x40000000 ? high_blank + 1 : 0;
    if (high_blank < kSegmentFrames)
      return false;
    high_blank = 0;
    return true;
  }
};

// Only an incomplete frame is copied. Complete frames remain in caller memory.
template <size_t Samples> class PcmFrames {
public:
  size_t available(size_t samples) const {
    require(samples <= size_t(PTRDIFF_MAX) - used_, "PCM length overflow");
    return (used_ + samples) / Samples;
  }
  size_t pending() const noexcept { return used_; }
  void clear() noexcept { used_ = 0; }
  template <class Consume>
  size_t push(const int16_t *pcm, size_t samples, Consume consume) {
    size_t frames = 0;
    if (used_) {
      size_t take = std::min(samples, Samples - used_);
      if (take)
        std::copy_n(pcm, take, tail_.data() + used_);
      used_ += take;
      if (take)
        pcm += take;
      samples -= take;
      if (used_ == Samples) {
        consume(tail_.data(), frames++);
        used_ = 0;
      }
    }
    while (samples >= Samples) {
      consume(pcm, frames++);
      pcm += Samples;
      samples -= Samples;
    }
    if (samples) {
      std::copy_n(pcm, samples, tail_.data());
      used_ = samples;
    }
    return frames;
  }
  const int16_t *padded() {
    std::fill(tail_.begin() + used_, tail_.end(), 0);
    return tail_.data();
  }

private:
  std::array<int16_t, Samples> tail_{};
  size_t used_ = 0;
};

inline OutputBuffers frame_output(OutputBuffers b, size_t frame, size_t classes,
                                  size_t keywords, size_t sensitivities) {
  if (b.q31)
    b.q31 += frame * classes;
  if (b.resets)
    b.resets += frame;
  if (b.scores)
    b.scores += frame * keywords;
  if (b.raw)
    b.raw += frame * keywords;
  if (b.fires)
    b.fires += frame * keywords * sensitivities;
  if (b.gates)
    b.gates += frame;
  b.capacity = 1;
  return b;
}
void validate_weights(const ModelFile &, const LanguagePreset &);
} // namespace kws::detail
