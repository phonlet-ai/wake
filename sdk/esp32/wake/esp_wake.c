#include "esp_wake.h"

#include "s3_engine.h"
#include "sdkconfig.h"

/* Frames per pass over the weights (menuconfig: Phonlet Wake): a batch of B
 * reads the weights from PSRAM once per B frames and delays detections by
 * (B - 1) x 32 ms. */
#ifndef ESP_WAKE_S3_BATCH
#ifdef CONFIG_ESP_WAKE_S3_BATCH
#define ESP_WAKE_S3_BATCH CONFIG_ESP_WAKE_S3_BATCH
#else
#define ESP_WAKE_S3_BATCH 4
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The session whose names esp_wake_keyword_name reports: the last one begun. */
static const pw_wake *named;

const char *esp_wake_keyword_name(int32_t index) {
  const char *name = phonlet_wake_keyword_name(named, index);
  return name[0] ? name : "?";
}

pw_status esp_wake_begin(const uint8_t *model_bytes, int32_t model_size,
                         const char *enrollments_json,
                         float sensitivity, pw_wake **wake) {
  if (model_bytes == NULL || enrollments_json == NULL || wake == NULL)
    return PW_ERR_INVALID_ARGUMENT;
  int keywords = phonlet_wake_keyword_count(enrollments_json);
  if (keywords < 1) {
    printf("esp_wake: no keywords: %s\n", phonlet_wake_error());
    return PW_ERR_INVALID_ARGUMENT;
  }
  /* The S3 engine: int8 weights on the vector unit, 16-bit activations. Its
   * kernel is checked against plain C first; on a mismatch the float path runs. */
  int mismatches = esp_wake_s3_selftest();
  if (mismatches == 0)
    esp_wake_s3_enable(16, ESP_WAKE_S3_BATCH);
  else
    esp_wake_s3_disable();
  printf("esp_wake: kernel %s self-test %s\n", esp_wake_s3_kernel(),
         mismatches ? "FAILED, using the float engine" : "passed");
  if (mismatches == 0)
    printf("esp_wake: batch %d frames (%d ms detection delay)\n", ESP_WAKE_S3_BATCH,
           (ESP_WAKE_S3_BATCH - 1) * 32);
  double *sensitivities = calloc((size_t)keywords, sizeof(double));
  if (sensitivities == NULL) return PW_ERR_RUNTIME;
  for (int i = 0; i < keywords; i++) sensitivities[i] = (double)sensitivity;
  pw_status status = phonlet_wake_init_bytes(model_bytes, model_size,
                                             enrollments_json, sensitivities,
                                             keywords, wake);
  free(sensitivities);
  if (status != PW_OK)
    printf("esp_wake: init failed: %s\n", phonlet_wake_error());
  if (status == PW_OK) {
    named = *wake;
    size_t internal = 0, external = 0;
    esp_wake_s3_memory(&internal, &external);
    printf("esp_wake: %d keywords ready (sensitivity %.2f); S3 weights %u KB internal, %u KB PSRAM\n",
           keywords, sensitivity, (unsigned)(internal / 1024), (unsigned)(external / 1024));
  }
  return status;
}

pw_status esp_wake_feed(pw_wake *wake, const int16_t *pcm, int32_t samples,
                        int32_t *fired) {
  if (fired == NULL) return PW_ERR_INVALID_ARGUMENT;
  *fired = -1;
  return phonlet_wake_process(wake, pcm, samples, fired);
}
