/*
 * Phonlet Wake — public C API.
 *
 * One tiny, stable surface for embedding wake-word detection: initialize with
 * a model container, an enrollment JSON array and one sensitivity per keyword,
 * stream mono 16 kHz PCM16 in arbitrary chunk sizes, read detections, release.
 *
 *   pw_wake            opaque session handle
 *   phonlet_wake_*     functions (brand + product + verb)
 *
 * Phrases come as the text of a keyword file, {"model": {...}, "keywords": [...]},
 * exactly as the threshold generator writes it, or as its bare "keywords" array.
 * Each phrase has "name", "pronunciations", "threshold_loose",
 * "threshold_strict" and "endpoint"; "model" and "metadata" are not read here.
 * The Node.js, browser and Python SDKs also check the file's model hash.
 *
 * This header is the supported public ABI. The engine's wider advanced
 * surface (batch evaluation, posterior access, custom scorers) is exported
 * separately under <kws/...> headers and is not covered by the public
 * compatibility promise.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque detector session: one continuous audio stream. */
typedef struct pw_wake pw_wake;

/** Result codes for every call that can fail. */
typedef enum pw_status {
  PW_OK = 0,
  PW_ERR_INVALID_ARGUMENT = 1,
  PW_ERR_INVALID_STATE = 2,
  PW_ERR_IO = 3,
  PW_ERR_RUNTIME = 4
} pw_status;

/**
 * Load a model container (.pwake) and create a detector session.
 *
 * @param model_path Path to a phonletwake model container.
 * @param enrollments_json A keyword file or a phrase array, as JSON text.
 * @param sensitivities One finite value in [0,1] per enrolled keyword.
 * @param count Number of enrolled keywords (and sensitivities).
 * @param wake Receives the session handle.
 * @return PW_OK, or an error code; phonlet_wake_error() has details.
 */
pw_status phonlet_wake_init(const char *model_path,
                            const char *enrollments_json,
                            const double *sensitivities,
                            int32_t count,
                            pw_wake **wake);

/**
 * Same as phonlet_wake_init, but the model container comes from memory
 * (e.g. flash-mapped firmware assets) instead of a filesystem path.
 */
pw_status phonlet_wake_init_bytes(const uint8_t *model_bytes,
                                  int32_t model_size,
                                  const char *enrollments_json,
                                  const double *sensitivities,
                                  int32_t count,
                                  pw_wake **wake);

/**
 * Process PCM in caller-chosen chunk sizes. The engine buffers incomplete
 * 512-sample frames; sensitivity arbitration picks at most one winner per
 * frame (production mode).
 *
 * @param keyword_index Receives the index of the first detection in this
 *   call (in the enrollment order), or -1 when none fired.
 */
pw_status phonlet_wake_process(pw_wake *wake, const int16_t *pcm,
                               int32_t samples, int32_t *keyword_index);

/** Close the audio stream: report discarded incomplete-tail samples. */
pw_status phonlet_wake_finish(pw_wake *wake, int32_t *discarded_samples);

/** Release the session. Null is ignored. */
void phonlet_wake_delete(pw_wake *wake);

/**
 * Number of phrases in a keyword file or phrase array, so a caller can size
 * the sensitivities; -1 when the JSON is invalid (phonlet_wake_error() says why).
 */
int32_t phonlet_wake_keyword_count(const char *keywords_json);

/** Name of phrase `index` in a session, as reported by phonlet_wake_process; "" if out of range. */
const char *phonlet_wake_keyword_name(const pw_wake *wake, int32_t index);

/** Engine ABI version of the public surface. */
int32_t phonlet_wake_version(void);

/** Model frame length in samples (chunks of any size are accepted). */
int32_t phonlet_wake_frame_length(void);

/** Required input sample rate: 16000 Hz, mono, signed PCM16. */
int32_t phonlet_wake_sample_rate(void);

/** Last error message for this thread; empty when the last call succeeded. */
const char *phonlet_wake_error(void);

#ifdef __cplusplus
}
#endif
