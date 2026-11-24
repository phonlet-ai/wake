#pragma once
#include <string>
#include <vector>
namespace kws {
struct LanguagePreset {
  std::string code;
  std::vector<std::string> symbols;
  std::string inventory_id;
  std::string metadata_json;
  int classes() const noexcept { return static_cast<int>(symbols.size()); }
};
const LanguagePreset &language_preset(const std::string &code = "en");
const std::vector<LanguagePreset> &supported_languages();
} // namespace kws
