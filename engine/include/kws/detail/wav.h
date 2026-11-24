#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kws::detail {

struct Wav {
  int sample_rate = 0;
  std::vector<int16_t> pcm;
};

Wav load_wav(const std::string &path);

std::vector<int16_t> load_raw_pcm(const std::string &path);

} // namespace kws::detail
