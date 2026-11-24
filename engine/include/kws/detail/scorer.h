#pragma once
#include <cstdint>
#include <vector>

#include "keyword.h"

namespace kws::detail {

int32_t log2q(int32_t p_q31);

struct Variant {
  explicit Variant(const std::vector<int> &ids);

#ifdef KWS_DEV
  struct Debug {
    int16_t score = 0;
    int32_t worst_window = 0;
    std::vector<int32_t> top_groups;
  };

  int16_t step(const int32_t *post40, Debug *dbg = nullptr);
#else
  int16_t step(const int32_t *post40);
#endif

  void reset();

  int n_states = 0, L = 0, max_dur = 0, window = 0, n_groups = 0;

private:
  std::vector<int32_t> states_;
  std::vector<uint8_t> can_skip_;
  std::vector<int32_t> sc_, ag_;
  std::vector<int32_t> tri_;
  std::vector<int32_t> tri_new_;
  std::vector<int32_t> emit_;
  int stuck_ = 0;
};

struct KeywordRuntime {
  explicit KeywordRuntime(const KeywordSpec &spec);

  void reset();

#ifdef KWS_DEV

  int16_t step(const int32_t *post40, const int32_t blank_cost3[3],
               std::vector<Variant::Debug> *dbg = nullptr);
#else
  int16_t step(const int32_t *post40, const int32_t blank_cost3[3]);
#endif

  KeywordSpec spec;
  int32_t threshold = 0;
  std::vector<Variant> variants;
  int32_t hist[3] = {-32768, -32768, -32768};
  int64_t counter = 0;

private:
  bool endpoint_;
};

constexpr int kResetFrames = 1875;
constexpr int kSegmentFrames = kResetFrames + 1;
constexpr int kDebounceFrames = 16;
constexpr int kEndpointMin = -385;
constexpr int kStuckLimit = 8;

} // namespace kws::detail
