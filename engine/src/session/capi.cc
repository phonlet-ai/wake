#include "internal.h"
#include "kernels.h"
#include "keyword_scorer.h"
#include "kws/runtime.h"
#include "kws/runtime.hpp"
#include "runtime_tables.h"
#include <limits>
#include <memory>
#include <string>

namespace {
thread_local std::string last_error;
using Weights = std::shared_ptr<const kws::ModelWeights>;

template <class Result, class Operation>
Result invoke(Result failure, Operation operation) noexcept {
  try {
    last_error.clear();
    return operation();
  } catch (const std::exception &error) {
    last_error = error.what();
  } catch (...) {
    last_error = "unknown native exception";
  }
  return failure;
}
template <class T> T &handle(void *pointer) {
  kws::detail::require(pointer != nullptr, "closed native handle");
  return *static_cast<T *>(pointer);
}
kws::KeywordMode keyword_mode(int mode) {
  kws::detail::require(mode == 0 || mode == 1, "invalid keyword mode");
  return mode == 0 ? kws::KeywordMode::independent
                   : kws::KeywordMode::production_arbitrated;
}
std::string language(const char *code) { return code ? code : "en"; }
} // namespace

extern "C" {
uint32_t kws_abi_version() { return 1; }
uint32_t kws_frontend_id() { return kws::detail::kFrontendId; }
const char *kws_error() { return last_error.c_str(); }
const char *kws_cpu_kernel() {
  return invoke<const char *>(nullptr,
                              [] { return kws::detail::selected_kernel(); });
}
const char *kws_language(const char *code) {
  return invoke<const char *>(nullptr, [&] {
    return kws::language_preset(language(code)).metadata_json.c_str();
  });
}
void *kws_model_load(const char *path, const char *code) {
  return invoke<void *>(nullptr, [&] {
    kws::detail::require(path != nullptr, "null model path");
    return new Weights(kws::ModelWeights::load(path, language(code)));
  });
}
void *kws_model_load_bytes(const uint8_t *bytes, int64_t count, const char *code) {
  return invoke<void *>(nullptr, [&] {
    size_t n = kws::detail::checked_count(count);
    kws::detail::require(bytes && n, "model bytes are required");
    return new Weights(kws::ModelWeights::from_bytes(std::vector<uint8_t>(bytes, bytes + n), language(code)));
  });
}
void *kws_model_tensors(uint32_t frontend, const int16_t *affine,
                        const int8_t *dense, int64_t count, const char *code) {
  return invoke<void *>(nullptr, [&] {
    return new Weights(kws::ModelWeights::from_tensors(
        frontend, affine, 80, dense, kws::detail::checked_count(count),
        language(code)));
  });
}
const char *kws_model_info(void *pointer) {
  return invoke<const char *>(nullptr, [&] {
    static thread_local std::string text;
    text = handle<Weights>(pointer)->info_json();
    return text.c_str();
  });
}
int kws_batch_run(void *model, int k, const int16_t *const *pcm, const int64_t *samples,
                  const char *enrollments, const double *sensitivities, int64_t count, int mode,
                  int32_t *const *q31, int16_t *const *scores, uint8_t *const *resets) {
  return invoke<int>(-1, [&] {
    const auto &weights = handle<Weights>(model);
    std::unique_ptr<kws::DetectorConfig> config;
    if (enrollments)
      config = std::make_unique<kws::DetectorConfig>(kws::parse_enrollments(
          enrollments, sensitivities, kws::detail::checked_count(count, sizeof(double)),
          keyword_mode(mode), weights->classes(), &weights->language()));
    kws::run_batch(weights, k, pcm, samples, config.get(), q31, scores, resets);
    return 0;
  });
}
const char *kws_scorers() {
  return invoke<const char *>(nullptr, [] {
    static thread_local std::string text;
    text = "[";
    for (const auto &name : kws::detail::scorer_names())
      text += (text.size() > 1 ? ",\"" : "\"") + name + "\"";
    text += "]";
    return text.c_str();
  });
}
void kws_model_destroy(void *pointer) {
  delete static_cast<Weights *>(pointer);
}
void *kws_frontend_create() {
  return invoke<void *>(nullptr, [] { return new kws::Frontend; });
}
void kws_frontend_destroy(void *pointer) {
  delete static_cast<kws::Frontend *>(pointer);
}
int kws_frontend_reset(void *pointer) {
  return invoke<int>(-1, [&] {
    handle<kws::Frontend>(pointer).reset();
    return 0;
  });
}
int64_t kws_frontend_push(void *pointer, const int16_t *pcm, int64_t samples,
                          int16_t *mel, int64_t capacity) {
  return invoke<int64_t>(-1, [&] {
    return handle<kws::Frontend>(pointer).push(
        pcm, kws::detail::checked_count(samples, 2), mel,
        kws::detail::checked_count(capacity, 80));
  });
}
int64_t kws_frontend_finish(void *pointer, int nearest, int16_t *mel) {
  return invoke<int64_t>(-1, [&] {
    kws::detail::require(nearest == 0 || nearest == 1,
                         "unknown frontend finalization");
    return handle<kws::Frontend>(pointer).finish(
        nearest ? kws::Finalization::training_nearest_hop
                : kws::Finalization::complete_hops,
        mel);
  });
}
void *kws_detector_create(const char *json, const double *sensitivities,
                          int64_t count, int mode, int classes,
                          const char *code) {
  return invoke<void *>(nullptr, [&] {
    return new kws::Detector(kws::parse_enrollments(
        json, sensitivities, kws::detail::checked_count(count, sizeof(double)),
        keyword_mode(mode), classes,
        code ? &kws::language_preset(code) : nullptr));
  });
}
void *kws_detector_create_per_keyword(const char *json, const double *sensitivities,
                          int64_t count, int mode, int classes, const char *code) {
  return invoke<void *>(nullptr, [&] {
    return new kws::Detector(kws::parse_enrollments_per_keyword(
        json, sensitivities, kws::detail::checked_count(count, sizeof(double)),
        keyword_mode(mode), classes, code ? &kws::language_preset(code) : nullptr));
  });
}
void kws_detector_destroy(void *pointer) {
  delete static_cast<kws::Detector *>(pointer);
}
int kws_detector_replay_scores(void *pointer, const int16_t *scores,
                               int64_t frames, uint8_t *events) {
  return invoke<int>(-1, [&] {
    auto &detector = handle<kws::Detector>(pointer);
    size_t stride =
        detector.keywords() * std::max(size_t(2), detector.sensitivities());
    detector.replay_scores(scores, kws::detail::checked_count(frames, stride),
                           events);
    return 0;
  });
}
int kws_detector_push(void *pointer, const int32_t *posteriors, int64_t frames,
                      const uint8_t *resets, int16_t *scores, int16_t *raw,
                      uint8_t *fires, uint8_t *gates) {
  return invoke<int>(-1, [&] {
    auto &detector = handle<kws::Detector>(pointer);
    size_t n = kws::detail::checked_count(frames,
                                          sizeof(int32_t) * detector.classes());
    detector.push(posteriors, n, resets,
                  {n, nullptr, nullptr, scores, raw, fires, gates});
    return 0;
  });
}
void *kws_session_create(void *pointer, const char *json,
                         const double *sensitivities, int64_t count, int mode) {
  return invoke<void *>(nullptr, [&] {
    const auto &weights = handle<Weights>(pointer);
    std::unique_ptr<kws::Detector> detector;
    if (json)
      detector = std::make_unique<kws::Detector>(kws::parse_enrollments(
          json, sensitivities,
          kws::detail::checked_count(count, sizeof(double)), keyword_mode(mode),
          weights->classes(), &weights->language()));
    return new kws::Session(weights, std::move(detector));
  });
}
void *kws_session_create_per_keyword(void *pointer, const char *json,
                         const double *sensitivities, int64_t count, int mode) {
  return invoke<void *>(nullptr, [&] {
    const auto &weights = handle<Weights>(pointer);
    auto detector = std::make_unique<kws::Detector>(kws::parse_enrollments_per_keyword(
        json, sensitivities, kws::detail::checked_count(count, sizeof(double)),
        keyword_mode(mode), weights->classes(), &weights->language()));
    return new kws::Session(weights, std::move(detector));
  });
}
void kws_session_destroy(void *pointer) {
  delete static_cast<kws::Session *>(pointer);
}
int64_t kws_session_finish(void *pointer) {
  return invoke<int64_t>(
      -1, [&] { return handle<kws::Session>(pointer).finish(); });
}
int64_t kws_session_push(void *pointer, const int16_t *input, int64_t count,
                         int cached_mel, int64_t capacity, int32_t *posteriors,
                         uint8_t *resets, int16_t *scores, int16_t *raw,
                         uint8_t *fires, uint8_t *gates) {
  return invoke<int64_t>(-1, [&] {
    kws::detail::require(cached_mel == 0 || cached_mel == 1,
                         "invalid input mode");
    size_t n = kws::detail::checked_count(count, cached_mel ? 160 : 2);
    kws::OutputBuffers outputs{kws::detail::checked_count(capacity),
                               posteriors,
                               resets,
                               scores,
                               raw,
                               fires,
                               gates};
    auto &session = handle<kws::Session>(pointer);
    return cached_mel ? session.push_mel(input, n, outputs)
                      : session.push_pcm(input, n, outputs);
  });
}
int kws_replay_scores(const int16_t *scores, int64_t frames, int64_t keywords,
                      const int16_t *thresholds, int64_t sensitivities,
                      int mode, uint8_t *events) {
  return invoke<int>(-1, [&] {
    size_t k = kws::detail::checked_count(keywords, 2);
    size_t s = kws::detail::checked_count(sensitivities);
    kws::detail::require(k && s && k <= size_t(PTRDIFF_MAX) / s,
                         "invalid replay dimensions");
    size_t n = kws::detail::checked_count(frames, k * std::max(size_t(2), s));
    kws::replay_scores(scores, n, k, thresholds, s, keyword_mode(mode), events);
    return 0;
  });
}
}
