#pragma once
#include <cstdint>

namespace kws::detail {

class Frontend {
public:
  Frontend();
  void reset();
  void process(const int16_t *pcm256, int16_t mel40[40]);

  static constexpr int kHop = 256;
  static constexpr int nMels = 40;

private:
  void fft512_real_(const float *w);

  float prev_sample_ = 0.0f;
  float history_[256] = {0};

  float frame_[512];
  float re_[256], im_[256];
  float power_[257];
};

} // namespace kws::detail
