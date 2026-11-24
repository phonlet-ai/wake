#pragma once

#include "kws/language.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kws {
namespace detail {
struct ModelFile;
struct FloatModelFile;
class Acoustic;
}

// Shared acoustic topology. The language preset supplies the output inventory.
struct AcousticContract {
  static constexpr int bands = 40;
  static constexpr int stacked_features = 80;
  static constexpr int english_classes = 40;
  static constexpr int layers = 6;
  static constexpr int hidden = 168;
  static constexpr int english_values = 984600;
};

enum class KeywordMode { independent, production_arbitrated };
enum class Finalization { complete_hops, training_nearest_hop };

struct Enrollment {
  std::string name;
  std::vector<std::vector<int>> pronunciations;
  float threshold_loose = 0;
  float threshold_strict = 0;
  bool endpoint = true;
  // engine-multi: stage-2 scorer by registry name (keyword_scorer.h) and its
  // option string; JSON fields "scorer" / "scorer_options", CLI --scorer.
  std::string scorer = "production";
  std::string scorer_options;
};

struct DetectorConfig {
  int classes = AcousticContract::english_classes;
  std::vector<Enrollment> enrollments;
  std::vector<double> sensitivities; // Legacy grid: one row per level.
  KeywordMode mode = KeywordMode::independent;
  std::vector<double> keyword_sensitivities; // Alternative: K values, one output row.
};

// Optional, caller-owned output planes. Capacity is measured in frames.
// q31: [frames,classes]; scores/raw: [frames,keywords];
// fires: [frames,sensitivities,keywords]; resets/gates: [frames].
struct OutputBuffers {
  size_t capacity = 0;
  int32_t *q31 = nullptr;
  uint8_t *resets = nullptr;
  int16_t *scores = nullptr;
  int16_t *raw = nullptr;
  uint8_t *fires = nullptr;
  uint8_t *gates = nullptr;
};

class ModelWeights final {
public:
  static std::shared_ptr<const ModelWeights>
  load(const std::string &path, const std::string &language = "en");
  static std::shared_ptr<const ModelWeights>
  from_tensors(uint32_t frontend_id, const int16_t *affine, size_t affine_count,
               const int8_t *dense, size_t dense_count,
               const std::string &language = "en");
  static std::shared_ptr<const ModelWeights>
  from_bytes(const std::vector<uint8_t> &bytes, const std::string &language = "en");
  ~ModelWeights();
  ModelWeights(const ModelWeights &) = delete;
  ModelWeights &operator=(const ModelWeights &) = delete;
  int classes() const noexcept;
  const LanguagePreset &language() const noexcept;
  // Read-only access for native diagnostics; ownership stays with this object.
  const detail::ModelFile &parameters() const;
  // Float-container models (any width, dense or low rank).
  bool is_float() const noexcept { return bool(float_); }
  const detail::FloatModelFile &float_parameters() const;
  std::unique_ptr<detail::Acoustic> make_acoustic() const;
  std::string info_json() const;

private:
  explicit ModelWeights(std::unique_ptr<detail::ModelFile>,
                        const LanguagePreset &);
  ModelWeights(std::unique_ptr<detail::FloatModelFile>, const LanguagePreset &);
  std::unique_ptr<const detail::ModelFile> parameters_;
  std::unique_ptr<const detail::FloatModelFile> float_;
  const LanguagePreset &language_;
};

class Frontend final {
public:
  Frontend();
  ~Frontend();
  Frontend(const Frontend &) = delete;
  Frontend &operator=(const Frontend &) = delete;
  void reset();
  size_t push(const int16_t *pcm, size_t samples, int16_t *mel,
              size_t capacity);
  size_t finish(Finalization policy, int16_t *mel);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Detector final {
public:
  explicit Detector(DetectorConfig config);
  ~Detector();
  Detector(const Detector &) = delete;
  Detector &operator=(const Detector &) = delete;
  size_t keywords() const noexcept;
  size_t sensitivities() const noexcept;
  int classes() const noexcept;
  void push(const int32_t *posteriors, size_t frames,
            const uint8_t *expected_resets, OutputBuffers outputs);
  // Score and fire are distinct so Session can reset acoustic/frontend state
  // between them. The last reported score survives an inactivity reset.
  void score_frame(const int32_t *posteriors, bool reset,
                   OutputBuffers outputs);
  void fire_frame(uint8_t *events);
  void replay_scores(const int16_t *scores, size_t frames,
                     uint8_t *events) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class Session final {
public:
  explicit Session(std::shared_ptr<const ModelWeights> weights,
                   std::unique_ptr<Detector> detector = nullptr);
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  size_t push_pcm(const int16_t *pcm, size_t samples, OutputBuffers outputs);
  size_t push_mel(const int16_t *mel, size_t frames, OutputBuffers outputs);
  size_t finish(); // Returns discarded PCM samples. Never pads deployment EOF.
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// engine-multi: K whole PCM streams in lockstep (see session.cc run_batch).
void run_batch(std::shared_ptr<const ModelWeights> weights, int k, const int16_t *const *pcm,
               const int64_t *samples, const DetectorConfig *config, int32_t *const *q31,
               int16_t *const *scores, uint8_t *const *resets);

DetectorConfig parse_enrollments(const char *json, const double *sensitivities,
                                 size_t count, KeywordMode mode, int classes,
                                 const LanguagePreset *language = nullptr);
DetectorConfig parse_enrollments_per_keyword(const char *json, const double *sensitivities,
                                 size_t count, KeywordMode mode, int classes,
                                 const LanguagePreset *language = nullptr);
// Phrases in a keyword file or a bare phrase array.
size_t count_enrollments(const char *json);
void replay_scores(const int16_t *scores, size_t frames, size_t keywords,
                   const int16_t *thresholds, size_t sensitivities,
                   KeywordMode mode, uint8_t *events);
} // namespace kws
