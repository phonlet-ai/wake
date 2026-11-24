#pragma once
/* ABI v1. All matrices are native-endian contiguous row-major arrays.
 * Inputs are borrowed for the duration of a call; outputs belong to the caller.
 * Handles are independent across threads; a single stream is not reentrant.
 * A stream retains immutable model weights after its factory handle is freed.
 * Null optional output pointers avoid materialization. No C++ exception
 * escapes. Frame indices are zero-based; frame t ends at (t+1)*512/16000
 * seconds.
 */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t kws_abi_version(void);
const char *kws_cpu_kernel(void);
uint32_t kws_frontend_id(void);
const char *kws_error(void);
/* A null language selects en. Preset JSON includes symbols, blank, class count
 * and inventory identity. The returned string is library-owned. */
const char *kws_language(const char *code);
void *kws_model_load(const char *path, const char *language);
/* Affine is 80 int16 values; dense is 977760 + 169*language_classes int8 values. */
void *kws_model_tensors(uint32_t frontend, const int16_t *affine80,
                        const int8_t *values, int64_t count,
                        const char *language);
/* Loads an owned copy from memory; input may be released after return. */
void *kws_model_load_bytes(const uint8_t *bytes, int64_t count, const char *language);
void kws_model_destroy(void *model);
/* JSON description of a loaded model (container kind, width,
 * parameters, per-layer structure and storage, kernel). Library-owned. */
const char *kws_model_info(void *model);
/* engine-multi: JSON array of the keyword scorer names built into this library;
 * select one per enrollment with "scorer"/"scorer_options". */
const char *kws_scorers(void);
/* engine-multi: k whole PCM streams in lockstep through one model; per stream
 * its own frontend, reset clock and detector (identical to one session per
 * stream). Optional per-stream outputs: q31 [frames,classes], scores
 * [frames,keywords], resets [frames]; frames = samples/512. */
int kws_batch_run(void *model, int k, const int16_t *const *pcm, const int64_t *samples,
                  const char *enrollments, const double *sensitivities, int64_t count, int mode,
                  int32_t *const *q31, int16_t *const *scores, uint8_t *const *resets);
void *kws_frontend_create(void);
void kws_frontend_destroy(void *frontend);
int kws_frontend_reset(void *frontend);
int64_t kws_frontend_push(void *frontend, const int16_t *pcm, int64_t samples,
                          int16_t *mel, int64_t capacity);
/* finish: nearest=1 pads a tail >=128 samples; nearest=0 discards the tail. */
int64_t kws_frontend_finish(void *frontend, int nearest, int16_t *mel);
/* enrollments is a JSON array of {name,pronunciations,threshold_loose,threshold_strict,endpoint}.
 * mode: 0 independent keywords; 1 production winner arbitration per
 * sensitivity. scores/raw: [frames,keywords], fires: uint8
 * [frames,sensitivities,keywords].
 */
void *kws_detector_create(const char *enrollments, const double *sensitivities,
                          int64_t count, int mode, int classes,
                          const char *language);
/* Per-keyword layout: count MUST equal K; fires are [frames,1,K].
 * Existing grid constructors retain their original meaning, even when S == K. */
void *kws_detector_create_per_keyword(const char *enrollments, const double *sensitivities,
                          int64_t count, int mode, int classes, const char *language);
void kws_detector_destroy(void *detector);
int kws_detector_replay_scores(void *detector, const int16_t *scores,
                               int64_t frames, uint8_t *events);
int kws_detector_push(void *detector, const int32_t *q31, int64_t frames,
                      const uint8_t *resets, int16_t *scores, int16_t *raw,
                      uint8_t *fires, uint8_t *gates);
void *kws_session_create(void *model, const char *enrollments,
                         const double *sensitivities, int64_t count, int mode);
void *kws_session_create_per_keyword(void *model, const char *enrollments,
                         const double *sensitivities, int64_t count, int mode);
void kws_session_destroy(void *session);
int64_t kws_session_finish(void *session);
/* Output capacity is in model frames. PCM tail is retained across pushes.
 * A session cannot mix PCM and cached-mel input. Destroy drops EOF tail.
 */
int64_t kws_session_push(void *session, const int16_t *input, int64_t count,
                         int cached_mel, int64_t capacity, int32_t *q31,
                         uint8_t *resets, int16_t *scores, int16_t *raw,
                         uint8_t *fires, uint8_t *gates);
int kws_replay_scores(const int16_t *scores, int64_t frames, int64_t keywords,
                      const int16_t *thresholds, int64_t sensitivities,
                      int mode, uint8_t *fires);
#ifdef __cplusplus
}
#endif
