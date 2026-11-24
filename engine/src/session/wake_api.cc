// Public facade over the engine session: the thin, stable surface declared in
// include/phonlet_wake.h. Sessions use production arbitration with one
// resolved sensitivity per keyword; the wider kws_* surface stays advanced.
#include "internal.h"
#include "phonlet_wake.h"
#include "kws/runtime.h"
#include "kws/runtime.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

thread_local std::string last_error;

template <class Operation>
pw_status invoke(Operation operation) noexcept {
  try {
    last_error.clear();
    return operation();
  } catch (const std::exception &error) {
    last_error = error.what();
  } catch (...) {
    last_error = "unknown native exception";
  }
  return PW_ERR_RUNTIME;
}

} // namespace

// Definition of the opaque public handle (global scope matches the header).
struct pw_wake {
  std::shared_ptr<const kws::ModelWeights> weights;
  std::unique_ptr<kws::Session> session;
  size_t keywords;
  std::vector<std::string> names;
  std::vector<uint8_t> fires;
};

namespace {
int32_t clamp_samples(int64_t value) {
  if (value < 0 || value > INT32_MAX)
    kws::detail::require(false, "sample count out of range");
  return (int32_t)value;
}

void check_sensitivities(const double *sensitivities, int32_t count) {
  kws::detail::require(count > 0, "at least one keyword is required");
  for (int32_t i = 0; i < count; ++i) {
    double value = sensitivities[i];
    kws::detail::require(std::isfinite(value) && value >= 0.0 && value <= 1.0,
                         "sensitivities must be finite values in [0,1]");
  }
}

pw_wake *create(std::shared_ptr<const kws::ModelWeights> weights, const char *keywords_json,
                const double *sensitivities, int32_t count) {
  auto config = kws::parse_enrollments_per_keyword(
      keywords_json, sensitivities, (size_t)count,
      kws::KeywordMode::production_arbitrated, weights->classes(), &weights->language());
  auto facade = std::make_unique<pw_wake>();
  for (const auto &enrollment : config.enrollments)
    facade->names.push_back(enrollment.name);
  auto detector = std::make_unique<kws::Detector>(std::move(config));
  facade->keywords = detector->keywords();
  facade->fires.assign(facade->keywords, 0);
  facade->session = std::make_unique<kws::Session>(weights, std::move(detector));
  facade->weights = std::move(weights);
  return facade.release();
}
} // namespace


extern "C" {

pw_status phonlet_wake_init(const char *model_path,
                            const char *enrollments_json,
                            const double *sensitivities,
                            int32_t count, pw_wake **wake) {
  return invoke([&]() -> pw_status {
    kws::detail::require(model_path && enrollments_json && sensitivities && wake,
                         "model, keywords, sensitivities and output handle are required");
    check_sensitivities(sensitivities, count);
    *wake = create(kws::ModelWeights::load(model_path, "en"), enrollments_json,
                   sensitivities, count);
    return PW_OK;
  });
}

pw_status phonlet_wake_init_bytes(const uint8_t *model_bytes,
                                  int32_t model_size,
                                  const char *enrollments_json,
                                  const double *sensitivities,
                                  int32_t count, pw_wake **wake) {
  return invoke([&]() -> pw_status {
    kws::detail::require(model_bytes && model_size > 0, "model bytes are required");
    kws::detail::require(enrollments_json && sensitivities && wake,
                         "keywords, sensitivities and output handle are required");
    check_sensitivities(sensitivities, count);
    *wake = create(kws::ModelWeights::from_bytes(
                       std::vector<uint8_t>(model_bytes, model_bytes + model_size), "en"),
                   enrollments_json, sensitivities, count);
    return PW_OK;
  });
}

int32_t phonlet_wake_keyword_count(const char *keywords_json) {
  int32_t count = -1;
  pw_status status = invoke([&]() -> pw_status {
    count = (int32_t)kws::count_enrollments(keywords_json);
    return PW_OK;
  });
  return status == PW_OK ? count : -1;
}

const char *phonlet_wake_keyword_name(const pw_wake *wake, int32_t index) {
  if (wake == nullptr || index < 0 || (size_t)index >= wake->names.size())
    return "";
  return wake->names[(size_t)index].c_str();
}

pw_status phonlet_wake_process(pw_wake *wake, const int16_t *pcm,
                               int32_t samples, int32_t *keyword_index) {
  return invoke([&]() -> pw_status {
    kws::detail::require(wake != nullptr, "closed native handle");
    kws::detail::require(keyword_index != nullptr, "keyword index output is required");
    kws::detail::require(samples >= 0 && (pcm || !samples), "invalid PCM input");
    *keyword_index = -1;
    auto &facade = *wake;
    const size_t k = facade.keywords;
    const size_t capacity = (size_t)samples / 512 + 1; // carried tail adds at most one frame
    facade.fires.assign(capacity * k, 0);
    kws::OutputBuffers out{capacity, nullptr, nullptr,
                           nullptr, nullptr, facade.fires.data(), nullptr};
    int64_t frames = facade.session->push_pcm(pcm, clamp_samples(samples), out);
    for (int64_t f = 0; f < frames && *keyword_index < 0; ++f)
      for (size_t i = 0; i < k; ++i)
        if (facade.fires[(size_t)f * k + i]) {
          *keyword_index = (int32_t)i;
          break;
        }
    return PW_OK;
  });
}

pw_status phonlet_wake_finish(pw_wake *wake, int32_t *discarded_samples) {
  return invoke([&]() -> pw_status {
    kws::detail::require(wake != nullptr, "closed native handle");
    kws::detail::require(discarded_samples != nullptr,
                         "discarded-sample output is required");
    *discarded_samples = clamp_samples(wake->session->finish());
    return PW_OK;
  });
}

void phonlet_wake_delete(pw_wake *wake) { delete wake; }

int32_t phonlet_wake_version(void) { return 1; }

int32_t phonlet_wake_frame_length(void) { return 512; }

int32_t phonlet_wake_sample_rate(void) { return 16000; }

const char *phonlet_wake_error(void) { return last_error.c_str(); }

} // extern "C"
