#include "keyword_scorer.h"

#include <mutex>
#include <stdexcept>
#include <utility>

#include "scorer.h"

namespace kws::detail {

namespace {

// The production lattice scorer, unchanged, behind the interface.
class ProductionScorer final : public KeywordScorer {
public:
  explicit ProductionScorer(const KeywordSpec &spec) : runtime_(spec) {}
  int16_t step(const int32_t *post, const int32_t blank_cost3[3]) override {
    return runtime_.step(post, blank_cost3);
  }
  int16_t raw() const override { return (int16_t)runtime_.hist[2]; }
  void reset() override { runtime_.reset(); }
  const KeywordSpec &spec() const override { return runtime_.spec; }

private:
  KeywordRuntime runtime_;
};

std::unique_ptr<KeywordScorer> make_production(const KeywordSpec &spec,
                                               const std::string &options) {
  if (!options.empty())
    throw std::invalid_argument("the production scorer takes no options");
  return std::make_unique<ProductionScorer>(spec);
}

struct Registry {
  std::mutex lock;
  std::vector<std::pair<std::string, ScorerFactory>> entries;
};

Registry &registry() {
  static Registry *r = [] {
    auto *out = new Registry;
    out->entries.emplace_back("production", make_production);
    out->entries.emplace_back("stage2", make_stage2_scorer);
    return out;
  }();
  return *r;
}

} // namespace

void register_scorer(const std::string &name, ScorerFactory factory) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  for (auto &e : r.entries)
    if (e.first == name)
      throw std::invalid_argument("scorer already registered: " + name);
  r.entries.emplace_back(name, factory);
}

std::unique_ptr<KeywordScorer> make_scorer(const std::string &name,
                                           const KeywordSpec &spec,
                                           const std::string &options) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  for (auto &e : r.entries)
    if (e.first == name)
      return e.second(spec, options);
  throw std::invalid_argument("unknown keyword scorer: " + name);
}

std::vector<std::string> scorer_names() {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  std::vector<std::string> out;
  for (auto &e : r.entries)
    out.push_back(e.first);
  return out;
}

bool has_scorer(const std::string &name) {
  for (auto &n : scorer_names())
    if (n == name)
      return true;
  return false;
}

} // namespace kws::detail
