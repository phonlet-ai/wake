/* Integer kernels of the ESP32-S3 acoustic engine (see s3_kernels.c). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** y[r] = sum_c w[r * cols + c] * x[c]; cols % 16 == 0, w and x 16-byte aligned. */
void s3_matvec_s8(const int8_t *w, int rows, int cols, const int8_t *x, int32_t *y);

/** The same products for several inputs, row by row: y[k][r] = row r . x[k]
 *  (each row is fetched from memory once). */
void s3_matvec_multi_s8(const int8_t *w, int rows, int cols, const int8_t *const *x, int inputs,
                        int32_t *const *y);

/** Plain C dot product, used to check the SIMD kernel. */
int32_t s3_dot_s8_reference(const int8_t *w, const int8_t *x, int cols);

/** "esp32s3-simd" on the chip, "portable-c" elsewhere. */
const char *s3_kernel_name(void);

#ifdef __cplusplus
}
#endif
