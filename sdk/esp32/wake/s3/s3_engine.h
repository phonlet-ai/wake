/* ESP32-S3 acoustic engine: control surface used by esp_wake.c and the host test. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Use the S3 engine for sessions created after this call. bits: 16 (two 8-bit
 *  passes, about 15-bit activations) or 8 (one pass). batch: frames computed per
 *  pass over the weights (1..8); posteriors are delayed by batch - 1 frames
 *  (32 ms each). Models it does not support keep the engine's float path. */
void esp_wake_s3_enable(int bits, int batch);
/** Back to the engine's float path for new sessions. */
void esp_wake_s3_disable(void);
/** "esp32s3-simd" on the chip, "portable-c" elsewhere. */
const char *esp_wake_s3_kernel(void);
/** Weight bytes placed in internal SRAM and in PSRAM so far. */
void esp_wake_s3_memory(size_t *internal_bytes, size_t *external_bytes);
/** CPU cycles spent in the integer kernels and in whole frames since the last
 *  call, and the frame count; resets the counters (zero off the chip). */
void esp_wake_s3_profile(uint32_t *kernel_cycles, uint32_t *total_cycles, uint32_t *frames);
/** Checks the SIMD dot product against plain C; returns the number of mismatches. */
int esp_wake_s3_selftest(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <memory>
#include "float_model.h"
/** Host tests: an S3 acoustic for `file` (null when unsupported). */
std::unique_ptr<kws::detail::Acoustic> esp_wake_s3_make(const kws::detail::FloatModelFile &file,
                                                        int bits, int batch);
#endif
