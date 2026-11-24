#include "internal.h"
#include "keyword.h"
#include "keyword_scorer.h"
#include "kws/runtime.hpp"
#include <array>
#include <climits>
#include <cmath>
namespace kws {
namespace {
void validate(const DetectorConfig &config) {
  detail::require(config.classes > 1 && config.classes <= 4096,
                  "invalid posterior class count");
  detail::require(!config.enrollments.empty(), "detector needs keywords");
  detail::require(config.sensitivities.empty() != config.keyword_sensitivities.empty(),
                  "select exactly one sensitivity layout");
  detail::require(config.keyword_sensitivities.empty() ||
                      config.keyword_sensitivities.size() == config.enrollments.size(),
                  "per-keyword sensitivity count must match keywords");
  detail::require(config.mode == KeywordMode::independent ||
                      config.mode == KeywordMode::production_arbitrated,
                  "invalid keyword mode");
  for (double sensitivity : config.keyword_sensitivities.empty()
                                ? config.sensitivities : config.keyword_sensitivities)
    detail::require(std::isfinite(sensitivity) && sensitivity >= 0 &&
                        sensitivity <= 1,
                    "sensitivity outside [0,1]");
  for (const auto &enrollment : config.enrollments) {
    detail::require(detail::has_scorer(enrollment.scorer),
                    "unknown keyword scorer (see --list-scorers)");
    detail::require(std::isfinite(enrollment.threshold_loose) &&
                        std::isfinite(enrollment.threshold_strict),
                    "nonfinite enrollment threshold");
    detail::require(!enrollment.pronunciations.empty(),
                    "keyword needs pronunciation pronunciations");
    for (const auto &variant : enrollment.pronunciations) {
      detail::require(!variant.empty() && variant.size() <= 256,
                      "invalid pronunciation length");
      for (int phone : variant)
        detail::require(phone > 0 && phone < config.classes,
                        "phone id outside declared posterior inventory");
    }
  }
}
void fire_frame(const int16_t *scores, size_t keywords,
                const int16_t *thresholds, size_t sensitivities,
                KeywordMode mode, std::vector<int> &counters, uint8_t *events) {
  if (events)
    std::fill_n(events, keywords * sensitivities, 0);
  for (size_t s = 0; s < sensitivities; ++s) {
    if (mode == KeywordMode::independent) {
      for (size_t k = 0; k < keywords; ++k) {
        size_t i = s * keywords + k;
        bool fire = counters[i] >= detail::kDebounceFrames && scores[k] >= thresholds[i];
        // Readiness saturates; preserving an unbounded integer is unnecessary.
        counters[i] = fire ? 0 : std::min(counters[i] + 1, detail::kDebounceFrames);
        if (fire && events)
          events[i] = 1;
      }
      continue;
    }
    // Arbitrated: of the ready phrases above their threshold, the one furthest above its own
    // threshold wins (ties go to the later phrase). Scores of different phrases are not
    // comparable on their own: each phrase has its own thresholds, and a phrase contained in
    // another ("assistant" in "hey assistant") often scores higher on the longer one's audio.
    size_t winner = keywords;
    int32_t best_margin = INT32_MIN;
    for (size_t k = 0; k < keywords; ++k) {
      size_t i = s * keywords + k;
      int32_t margin = int32_t(scores[k]) - thresholds[i];
      if (counters[i] >= detail::kDebounceFrames && margin >= 0 && margin >= best_margin) {
        winner = k;
        best_margin = margin;
      }
      counters[i] = std::min(counters[i] + 1, detail::kDebounceFrames);
    }
    if (winner == keywords)
      continue;
    // One utterance, one detection: every phrase waits out the debounce after a detection.
    for (size_t k = 0; k < keywords; ++k)
      counters[s * keywords + k] = 0;
    if (events)
      events[s * keywords + winner] = 1;
  }
}
} // namespace
struct Detector::Impl {
  DetectorConfig config;
  std::vector<std::unique_ptr<detail::KeywordScorer>> lattices;
  std::vector<int16_t> scores, thresholds;
  std::vector<int> counters;
  std::array<int32_t, 3> blank_history{{-32768, -32768, -32768}};
  detail::ResetClock clock;
  explicit Impl(DetectorConfig value) : config(std::move(value)) {
    validate(config);
    for (const auto &e : config.enrollments) {
      detail::KeywordSpec spec;
      spec.name = e.name;
      spec.pronunciations = e.pronunciations;
      spec.endpoint = e.endpoint;
      spec.threshold_loose = e.threshold_loose;
      spec.threshold_strict = e.threshold_strict;
      spec.scorer = e.scorer;
      spec.scorer_options = e.scorer_options;
      spec.classes = config.classes;
      lattices.push_back(detail::make_scorer(e.scorer, spec, e.scorer_options));
    }
    if (!config.keyword_sensitivities.empty()) {
      for (size_t k = 0; k < lattices.size(); ++k)
        thresholds.push_back(detail::threshold_for(lattices[k]->spec(), config.keyword_sensitivities[k]));
    } else {
      for (double sensitivity : config.sensitivities)
        for (const auto &lattice : lattices)
          thresholds.push_back(detail::threshold_for(lattice->spec(), sensitivity));
    }
    scores.resize(lattices.size());
    counters.resize(thresholds.size());
  }
};
Detector::Detector(DetectorConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
Detector::~Detector() = default;
size_t Detector::keywords() const noexcept { return impl_->lattices.size(); }
size_t Detector::sensitivities() const noexcept {
  return impl_->config.keyword_sensitivities.empty() ? impl_->config.sensitivities.size() : 1;
}
int Detector::classes() const noexcept { return impl_->config.classes; }
void Detector::score_frame(const int32_t *post, bool reset, OutputBuffers out) {
  auto &history = impl_->blank_history;
  history[0] = history[1];
  history[1] = history[2];
  history[2] = -detail::log2q(post[0]);
  if (out.gates)
    *out.gates =
        int64_t(history[0]) + history[1] + history[2] > detail::kEndpointMin;
  for (size_t k = 0; k < keywords(); ++k) {
    auto &lattice = impl_->lattices[k];
    impl_->scores[k] = lattice->step(post, history.data());
    if (out.scores)
      out.scores[k] = impl_->scores[k];
    if (out.raw)
      out.raw[k] = lattice->raw();
  }
  if (reset) {
    for (auto &lattice : impl_->lattices)
      lattice->reset();
    history.fill(-32768);
  }
}
void Detector::fire_frame(uint8_t *events) {
  kws::fire_frame(impl_->scores.data(), keywords(), impl_->thresholds.data(),
                  sensitivities(), impl_->config.mode, impl_->counters, events);
}
void Detector::push(const int32_t *post, size_t frames, const uint8_t *expected,
                    OutputBuffers out) {
  detail::require(post || !frames, "null posterior buffer");
  detail::require(out.capacity >= frames, "output buffer too small");
  auto validation_clock = impl_->clock;
  for (size_t i = 0; i < frames; ++i) {
    const auto *row = post + i * classes();
    for (int c = 0; c < classes(); ++c)
      detail::require(row[c] >= 0, "negative Q31 posterior");
    bool reset = validation_clock.advance(row[0]);
    detail::require(!expected || expected[i] == reset,
                    "posterior reset context mismatch");
  }
  for (size_t i = 0; i < frames; ++i) {
    const auto *row = post + i * classes();
    auto outputs =
        detail::frame_output(out, i, classes(), keywords(), sensitivities());
    bool reset = impl_->clock.advance(row[0]);
    score_frame(row, reset, outputs);
    fire_frame(outputs.fires);
    if (outputs.resets)
      *outputs.resets = reset;
    if (outputs.q31)
      std::copy_n(row, classes(), outputs.q31);
  }
}
void replay_scores(const int16_t *scores, size_t frames, size_t keywords,
                   const int16_t *thresholds, size_t sensitivities,
                   KeywordMode mode, uint8_t *events) {
  detail::require(keywords && sensitivities && thresholds &&
                      (!frames || (scores && events)),
                  "invalid score replay");
  std::vector<int> counters(keywords * sensitivities);
  for (size_t i = 0; i < frames; ++i)
    fire_frame(scores + i * keywords, keywords, thresholds, sensitivities, mode,
               counters, events + i * keywords * sensitivities);
}
void Detector::replay_scores(const int16_t *scores, size_t frames,
                             uint8_t *events) const {
  kws::replay_scores(scores, frames, keywords(), impl_->thresholds.data(),
                     sensitivities(), impl_->config.mode, events);
}
} // namespace kws
