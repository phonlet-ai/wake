#pragma once
// Stage-2 keyword scorer interface. The Detector owns one scorer per enrolled
// keyword and only ever talks to it through this interface, so a scorer can
// be replaced without touching the detector, the fire policy or the session.
//
// Contract, per frame:
//   step(post, blank_cost3) -> reported int16 score (higher = more keyword;
//                              -32768 = no valid score) used by the fire policy
//   raw()                   -> the scorer's own last raw score (diagnostics)
//   reset()                 -> the engine's inactivity reset (same clock as the
//                              acoustic reset); the reported score of the frame
//                              that triggered it has already been returned
// Scorers are created by name from a registry; "production" is the default and
// is the unchanged lattice scorer of engine/src/detector/scorer.cc.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "keyword.h"

namespace kws::detail {

class KeywordScorer {
public:
  virtual ~KeywordScorer() = default;
  virtual int16_t step(const int32_t *post, const int32_t blank_cost3[3]) = 0;
  virtual int16_t raw() const = 0;
  virtual void reset() = 0;
  virtual const KeywordSpec &spec() const = 0;
};

using ScorerFactory = std::unique_ptr<KeywordScorer> (*)(const KeywordSpec &,
                                                         const std::string &options);

// Registered names, in registration order. Registration happens at static
// initialisation inside the library (see scorer_registry.cc).
void register_scorer(const std::string &name, ScorerFactory factory);
std::unique_ptr<KeywordScorer> make_scorer(const std::string &name,
                                           const KeywordSpec &spec,
                                           const std::string &options = "");
std::vector<std::string> scorer_names();
bool has_scorer(const std::string &name);

// Built-in implementations (registered by scorer_registry.cc).
std::unique_ptr<KeywordScorer> make_stage2_scorer(const KeywordSpec &spec,
                                                  const std::string &options);

} // namespace kws::detail
