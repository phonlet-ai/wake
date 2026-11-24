#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kws::detail {

struct KeywordSpec {
  std::string name;
  bool endpoint = true;
  float threshold_loose = 0.f, threshold_strict = 0.f;
  std::vector<std::vector<int>> pronunciations;
  // engine-multi: which KeywordScorer runs this keyword (keyword_scorer.h),
  // its option string, and the posterior width it will be fed.
  std::string scorer = "production";
  std::string scorer_options;
  int classes = 0;
};

KeywordSpec load_keyword(const std::string &path);
KeywordSpec load_keyword_bin(const std::string &path);
KeywordSpec load_keyword_json(const std::string &path);
void save_keyword_bin(const KeywordSpec &spec, const std::string &path);
void save_keyword_json(const KeywordSpec &spec, const std::string &path);

int16_t threshold_for(const KeywordSpec &spec, double sensitivity);

} // namespace kws::detail
