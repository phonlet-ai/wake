#include "kws/language.hpp"
#include "languages.generated.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace kws {
const std::vector<LanguagePreset> &supported_languages() {
  return detail::language_presets;
}
const LanguagePreset &language_preset(const std::string &name) {
  std::string code = name;
  std::transform(code.begin(), code.end(), code.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  for (const auto &preset : supported_languages())
    if (preset.code == code)
      return preset;
  throw std::invalid_argument("unknown language '" + name +
                              "'; use --list-languages");
}
} // namespace kws
