#include "internal.h"
#include "json.h"
#include "kws/runtime.hpp"
#include <cmath>
#include <cstring>
namespace kws {
namespace {
// A keyword file ({"keywords": [...], optional "model" and "metadata"}) or its bare
// "keywords" array. "model" and "metadata" are for the SDKs and people; the engine
// reads only the phrases.
const detail::json::Value &keyword_list(const detail::json::Value &root) {
  const detail::json::Value &list = root.is_object() ? root.get("keywords") : root;
  detail::require(list.is_array() && list.size() > 0,
                  "keywords must be a keyword file with a nonempty \"keywords\" array, "
                  "or that array");
  return list;
}
detail::json::Value parse_text(const char *text) {
  return detail::json::parse(std::vector<uint8_t>(text, text + std::strlen(text)));
}
} // namespace

size_t count_enrollments(const char *text) {
  detail::require(text != nullptr, "keywords are required");
  return keyword_list(parse_text(text)).size();
}

DetectorConfig parse_enrollments(const char *text, const double *sensitivities,
                                 size_t count, KeywordMode mode, int classes,
                                 const LanguagePreset *language) {
  detail::require(text && sensitivities && count > 0,
                  "enrollment and sensitivities are required");
  detail::require(!language || language->classes() == classes,
                  "posterior width differs from selected language");
  auto json = parse_text(text);
  DetectorConfig config;
  config.classes = classes;
  config.mode = mode;
  config.sensitivities.assign(sensitivities, sensitivities + count);
  for (const auto &item : keyword_list(json).items()) {
    if (language && item.has("lang"))
      detail::require(language_preset(item.get("lang").as_string()).code ==
                          language->code,
                      "enrollment language differs from selected preset");
    if (language && item.has("inventory_id"))
      detail::require(item.get("inventory_id").as_string() ==
                          language->inventory_id,
                      "enrollment phone inventory mismatch");
    Enrollment enrollment;
    enrollment.name = item.get("name").as_string();
    enrollment.endpoint = item.get("endpoint").as_bool();
    enrollment.threshold_loose = item.get("threshold_loose").as_number();
    enrollment.threshold_strict = item.get("threshold_strict").as_number();
    if (item.has("scorer"))
      enrollment.scorer = item.get("scorer").as_string();
    if (item.has("scorer_options"))
      enrollment.scorer_options = item.get("scorer_options").as_string();
    for (const auto &variant : item.get("pronunciations").items()) {
      std::vector<int> phones;
      for (const auto &phone : variant.items()) {
        double id;
        if (phone.kind() == detail::json::Value::Kind::String) {
          detail::require(language != nullptr,
                          "symbolic phones require a language preset");
          const auto &symbol = phone.as_string();
          auto found = std::find(language->symbols.begin(),
                                 language->symbols.end(), symbol);
          detail::require(found != language->symbols.end(),
                          "unknown phone symbol in selected language");
          id = std::distance(language->symbols.begin(), found);
        } else {
          id = phone.as_number();
        }
        detail::require(std::isfinite(id) && id >= 1 && id < classes &&
                            std::floor(id) == id,
                        "phone id outside declared posterior inventory");
        phones.push_back(static_cast<int>(id));
      }
      enrollment.pronunciations.push_back(std::move(phones));
    }
    config.enrollments.push_back(std::move(enrollment));
  }
  return config;
}
DetectorConfig parse_enrollments_per_keyword(const char *text, const double *values,
                                 size_t count, KeywordMode mode, int classes,
                                 const LanguagePreset *language) {
  auto config = parse_enrollments(text, values, count, mode, classes, language);
  detail::require(count == config.enrollments.size(),
                  "per-keyword sensitivity count must match keywords");
  config.keyword_sensitivities = std::move(config.sensitivities);
  config.sensitivities.clear();
  return config;
}
} // namespace kws
