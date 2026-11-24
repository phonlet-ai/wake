#include "scorer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "runtime_tables.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace kws::detail {

namespace {
inline int32_t clamp16(int32_t x) {
  return x < -32768 ? -32768 : x > 32767 ? 32767 : x;
}

// Leading zero bits of v > 0.
inline int clz64(uint64_t v) {
#if defined(_MSC_VER)
  unsigned long index;
  _BitScanReverse64(&index, v);
  return 63 - static_cast<int>(index);
#else
  return __builtin_clzll(v);
#endif
}

inline int64_t floordiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if (a % b != 0 && ((a < 0) != (b < 0)))
    --q;
  return q;
}
} // namespace

// p_q31 is a Q31 probability (0 <= p < 2^31).
int32_t log2q(int32_t p_q31) {

  if (p_q31 <= 0)
    return 32767;
  uint64_t v = (uint64_t)p_q31 << 32;
  v >>= 1;
  // Shift the top bit up to bit 61 in one step (sh counts the shifts from -1),
  // then look the mantissa up in the shared table: no double maths per call,
  // which matters on FPUs without double precision (ESP32-S3).
  const int lead = clz64(v);
  const int shifts = lead > 2 ? lead - 2 : 0;
  v <<= shifts;
  const int sh = shifts - 1;
  uint64_t ii = (v + 2251799813685248ULL) >> 52;
  int32_t idx = (int32_t)ii - 512;

  int32_t r = quant_log_table()[idx] - sh * kQuantOctaveStep;
  if (r < -32767)
    return 32767;
  return -clamp16(r);
}

Variant::Variant(const std::vector<int> &ids) {
  L = (int)ids.size();
  n_states = 2 * L;
  max_dur = 5 * L;
  window = L < 7 ? L : 7;
  n_groups = L;
  states_.assign(n_states, 0);
  for (int i = 0; i < L; ++i)
    states_[2 * i] = ids[i];
  can_skip_.assign(n_states, 0);
  for (int j = 2; j < n_states; ++j)
    can_skip_[j] = states_[j] != states_[j - 2];
  sc_.assign(n_states, 0);
  ag_.assign(n_states, 0);
  tri_.assign((size_t)n_states * n_groups, 0);
  tri_new_.assign((size_t)n_states * n_groups, 0);
  emit_.assign(n_states, 0);
  reset();
}

void Variant::reset() {
  std::fill(sc_.begin(), sc_.end(), 32767);
  std::fill(ag_.begin(), ag_.end(), max_dur + 1);
  std::fill(tri_.begin(), tri_.end(), 32767);
  stuck_ = 0;
}

#ifdef KWS_DEV
int16_t Variant::step(const int32_t *post40, Debug *dbg) {
#else
int16_t Variant::step(const int32_t *post40) {
#endif
  for (int j = 0; j < n_states; ++j)
    emit_[j] = log2q(post40[states_[j]]);

  int best_top = n_states - 1;
  for (int j = n_states - 1; j >= 1; --j) {
    int best = j;
    if (sc_[j - 1] < sc_[best])
      best = j - 1;
    if (j != 1 && can_skip_[j] && sc_[j - 2] < sc_[best])
      best = j - 2;
    if (j == n_states - 1)
      best_top = best;

    int32_t nsc = clamp16(sc_[best] + emit_[j]);
    int64_t nag = (int64_t)ag_[best] + 1;
    if (nag > 2147483647LL)
      nag = 2147483647LL;

    memcpy(&tri_new_[(size_t)j * n_groups], &tri_[(size_t)best * n_groups],
           sizeof(int32_t) * n_groups);
    int gj = j >> 1, gb = best >> 1;
    int32_t cur = gj == gb ? tri_new_[(size_t)j * n_groups + gj] : 0;
    tri_new_[(size_t)j * n_groups + gj] = clamp16(cur + emit_[j]);

    sc_[j] = nsc;
    ag_[j] = (int32_t)nag;
  }

  sc_[0] = emit_[0];
  ag_[0] = 1;
  int32_t *row0 = &tri_new_[0];
  for (int g = 0; g < n_groups; ++g)
    row0[g] = 32767;
  row0[0] = emit_[0];

  stuck_ = best_top == n_states - 1 ? stuck_ + 1 : 1;

  const int32_t *top = &tri_new_[(size_t)(n_states - 1) * n_groups];
  int32_t best_win = -32768;
  for (int s = 0; s + window <= n_groups; ++s) {
    int64_t sum = 0;
    for (int k = 0; k < window; ++k)
      sum += top[s + k];
    int32_t m = (int32_t)clamp16((int32_t)floordiv(sum, window));
    if (m > best_win)
      best_win = m;
  }
  if (best_win < 0)
    best_win = 0;

  bool dead = ag_[n_states - 1] > max_dur || stuck_ >= kStuckLimit;
  tri_.swap(tri_new_);
#ifdef KWS_DEV
  if (dbg) {
    dbg->score = (int16_t)(dead ? -32768 : -best_win);
    dbg->worst_window = dead ? 0 : best_win;
    dbg->top_groups.assign(top, top + n_groups);
  }
#endif
  return (int16_t)(dead ? -32768 : -best_win);
}

KeywordRuntime::KeywordRuntime(const KeywordSpec &s) : spec(s) {
  variants.reserve(spec.pronunciations.size());
  for (const auto &v : spec.pronunciations)
    variants.emplace_back(v);
  endpoint_ = spec.endpoint;
  reset();
}

void KeywordRuntime::reset() {
  for (auto &v : variants)
    v.reset();
  hist[0] = hist[1] = hist[2] = -32768;
}

#ifdef KWS_DEV
int16_t KeywordRuntime::step(const int32_t *post40,
                             const int32_t blank_cost3[3],
                             std::vector<Variant::Debug> *dbg) {
#else
int16_t KeywordRuntime::step(const int32_t *post40,
                             const int32_t blank_cost3[3]) {
#endif
  int32_t raw = -32768;
#ifdef KWS_DEV
  if (dbg)
    dbg->clear();
#endif
  for (auto &v : variants) {
#ifdef KWS_DEV
    if (dbg) {
      dbg->emplace_back();
      int16_t s = v.step(post40, &dbg->back());
      if (s > raw)
        raw = s;
    } else
#endif
    {
      int32_t s = v.step(post40);
      if (s > raw)
        raw = s;
    }
  }

  int32_t reported = raw;
  if (endpoint_) {
    int64_t bl = (int64_t)blank_cost3[0] + blank_cost3[1] + blank_cost3[2];
    reported = bl > kEndpointMin ? hist[0] : (int32_t)-32768;
  }
  hist[0] = hist[1];
  hist[1] = hist[2];
  hist[2] = raw;
  return (int16_t)reported;
}

} // namespace kws::detail
