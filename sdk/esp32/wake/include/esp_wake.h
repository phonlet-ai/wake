/* ESP-IDF convenience layer over the public phonlet_wake C API.
 *
 * The typical firmware embeds the model container and a keyword file in flash
 * (EMBED_FILES) and starts detection from an I2S microphone at 16 kHz:
 *
 *   extern const uint8_t model_start[] asm("_binary_model_pwake_start");
 *   extern const uint8_t keywords_start[] asm("_binary_keywords_json_start");
 *   pw_wake *wake = NULL;
 *   esp_wake_begin(model_start, model_end - model_start,
 *                  (const char *)keywords_start, 0.5f, &wake);
 *   ... esp_wake_feed(wake, pcm, samples, &fired) per I2S read ...
 */
#pragma once

#include <stdint.h>
#include "phonlet_wake.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One shared sensitivity for every phrase in the embedded keyword file (or phrase array). */
pw_status esp_wake_begin(const uint8_t *model_bytes, int32_t model_size,
                         const char *enrollments_json,
                         float sensitivity, pw_wake **wake);

/** Run detection over one buffer of PCM16. `fired` receives the index of the
 *  keyword that fired in this call, or -1. */
pw_status esp_wake_feed(pw_wake *wake, const int16_t *pcm, int32_t samples,
                        int32_t *fired);

/** Name of a phrase of the last session begun, in file order ("?" when out of range). */
const char *esp_wake_keyword_name(int32_t index);

#ifdef __cplusplus
}
#endif
