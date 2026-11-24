// "stage2" keyword scorer: the recommended stage-2 scorer of
// results/stage2_research/README.md (production form in
// experiments/stage2_research/production), as a self-contained implementation
// of the KeywordScorer interface. Differences from "production":
//   - per frame, blank's probability is scaled by bscale and the row
//     renormalised; every emission cost is reduced by gop x the cost of the
//     frame's best phone (floored at 0);
//   - a path may take maxdur frames per phone;
//   - an expired (age >= max_dur) or unformed predecessor cannot win the
//     Viterbi comparison.
// The reset clock and the endpoint history keep the raw acoustic blank
// (the detector passes them unchanged). Options: "maxdur=8,gop=0.1,bscale=0.7".
#include "keyword_scorer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "scorer.h"

namespace kws::detail {

namespace {

inline int32_t clamp16(int32_t x) {
  return x < -32768 ? -32768 : x > 32767 ? 32767 : x;
}
inline int64_t floordiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if (a % b != 0 && ((a < 0) != (b < 0)))
    --q;
  return q;
}
inline int32_t to_q31(double p) {
  double q = std::floor(p * 2147483648.0 + 0.5);
  if (q < 0)
    q = 0;
  if (q > 2147483647.0)
    q = 2147483647.0;
  return (int32_t)q;
}

struct Settings {
  int maxdur = 8;
  double gop = 0.1;
  double bscale = 0.7;
};

Settings parse(const std::string &options) {
  Settings s;
  std::stringstream in(options);
  std::string item;
  while (std::getline(in, item, ',')) {
    if (item.empty())
      continue;
    auto eq = item.find('=');
    if (eq == std::string::npos)
      throw std::invalid_argument("stage2 scorer option needs name=value: " + item);
    std::string key = item.substr(0, eq);
    char *end = nullptr;
    double value = std::strtod(item.c_str() + eq + 1, &end);
    if (!end || *end)
      throw std::invalid_argument("stage2 scorer option value: " + item);
    if (key == "maxdur" && value >= 1 && value <= 64 && value == std::floor(value))
      s.maxdur = (int)value;
    else if (key == "gop" && value >= 0 && value <= 1)
      s.gop = value;
    else if (key == "bscale" && value > 0 && value <= 1)
      s.bscale = value;
    else
      throw std::invalid_argument("stage2 scorer option unknown or out of range: " + item);
  }
  return s;
}

class Stage2Variant {
public:
  Stage2Variant(const std::vector<int> &ids, int maxdur) {
    L = (int)ids.size();
    n_states = 2 * L;
    max_dur = maxdur * L;
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
  void reset() {
    std::fill(sc_.begin(), sc_.end(), 32767);
    std::fill(ag_.begin(), ag_.end(), max_dur + 1);
    std::fill(tri_.begin(), tri_.end(), 32767);
    stuck_ = 0;
  }
  int16_t step(const int32_t *row, int32_t cut) {
    for (int j = 0; j < n_states; ++j)
      emit_[j] = std::max<int32_t>(0, log2q(row[states_[j]]) - cut);
    int best_top = n_states - 1;
    for (int j = n_states - 1; j >= 1; --j) {
      int best = -1;
      auto consider = [&](int candidate) {
        if (ag_[candidate] < max_dur && sc_[candidate] < 32767 &&
            (best < 0 || sc_[candidate] < sc_[best]))
          best = candidate;
      };
      consider(j);
      consider(j - 1);
      if (j != 1 && can_skip_[j])
        consider(j - 2);
      if (j == n_states - 1)
        best_top = best;
      if (best < 0) {
        sc_[j] = 32767;
        ag_[j] = max_dur + 1;
        std::fill_n(&tri_new_[(size_t)j * n_groups], n_groups, 32767);
        continue;
      }
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
    return (int16_t)(dead ? -32768 : -best_win);
  }

private:
  int n_states = 0, L = 0, max_dur = 0, window = 0, n_groups = 0;
  std::vector<int32_t> states_;
  std::vector<uint8_t> can_skip_;
  std::vector<int32_t> sc_, ag_, tri_, tri_new_, emit_;
  int stuck_ = 0;
};

class Stage2Scorer final : public KeywordScorer {
public:
  Stage2Scorer(const KeywordSpec &spec, const Settings &s) : spec_(spec), s_(s) {
    if (spec_.classes < 2)
      throw std::invalid_argument("stage2 scorer needs the posterior class count");
    classes_ = spec_.classes;
    row_.assign(classes_, 0);
    for (const auto &v : spec_.pronunciations)
      variants_.emplace_back(v, s_.maxdur);
    reset();
  }
  int16_t step(const int32_t *post, const int32_t blank_cost3[3]) override {
    // Evidence row: blank scaled, renormalised; cut = gop x cost(best phone).
    const double blank = post[0] / 2147483648.0;
    const double z = 1.0 - blank + s_.bscale * blank;
    int32_t best = 0;
    for (int c = 0; c < classes_; ++c) {
      double p = post[c] / 2147483648.0;
      if (c == 0)
        p *= s_.bscale;
      row_[c] = to_q31(p / z);
      if (c > 0)
        best = std::max(best, row_[c]);
    }
    const int32_t cut = (int32_t)std::lround(s_.gop * log2q(best));
    int32_t raw = -32768;
    for (auto &v : variants_) {
      int32_t sc = v.step(row_.data(), cut);
      if (sc > raw)
        raw = sc;
    }
    int32_t reported = raw;
    if (spec_.endpoint) {
      int64_t bl = (int64_t)blank_cost3[0] + blank_cost3[1] + blank_cost3[2];
      reported = bl > kEndpointMin ? hist_[0] : (int32_t)-32768;
    }
    hist_[0] = hist_[1];
    hist_[1] = hist_[2];
    hist_[2] = raw;
    return (int16_t)reported;
  }
  int16_t raw() const override { return (int16_t)hist_[2]; }
  void reset() override {
    for (auto &v : variants_)
      v.reset();
    hist_[0] = hist_[1] = hist_[2] = -32768;
  }
  const KeywordSpec &spec() const override { return spec_; }

private:
  KeywordSpec spec_;
  Settings s_;
  std::vector<Stage2Variant> variants_;
  std::vector<int32_t> row_;
  int32_t hist_[3] = {-32768, -32768, -32768};
  int classes_ = 0;
};

} // namespace

std::unique_ptr<KeywordScorer> make_stage2_scorer(const KeywordSpec &spec,
                                                  const std::string &options) {
  return std::make_unique<Stage2Scorer>(spec, parse(options));
}

} // namespace kws::detail
