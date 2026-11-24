/* Integer dot-product kernels of the ESP32-S3 acoustic engine.
 *
 * y[r] = sum_c w[r * cols + c] * x[c] over int8 values with an exact int32 sum.
 * cols is a multiple of 16 and both w and x are 16-byte aligned (the engine pads
 * every row with zeros). On the ESP32-S3 each block of 16 products is one
 * EE.VMULAS.S8.ACCX instruction; elsewhere a C loop gives the same integers, so
 * the PC reference and the firmware agree bit for bit. */
#include "s3_kernels.h"

#include <stddef.h>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3)

#include "esp32s3/rom/cache.h"
#include "esp_memory_utils.h"

static inline int32_t dot_s8(const int8_t *w, const int8_t *x, int blocks) {
  int32_t sum;
  /* ACCX is the 40-bit accumulator of the S3 vector unit; the products of one
   * row never exceed 32 bits (128 * 128 * cols), so ACCX_0 holds the sum.
   * Software-pipelined: each multiply loads the next weight block, so loads and
   * products overlap. An odd block count runs as one block preloaded, pairs in
   * the loop, and the last block after it; an even count peels one block. */
  int pairs;
  if ((blocks & 1) == 0) {
    __asm__ volatile(
        "ee.zero.accx\n"
        "ee.vld.128.ip q0, %[w], 16\n"
        "ee.vld.128.ip q1, %[x], 16\n"
        "ee.vmulas.s8.accx q0, q1\n"
        : [w] "+r"(w), [x] "+r"(x)
        :
        : "memory");
    blocks -= 1;
    pairs = blocks / 2;
    __asm__ volatile(
        "ee.vld.128.ip q0, %[w], 16\n"
        "ee.vld.128.ip q1, %[x], 16\n"
        "loopnez %[n], 1f\n"
        "ee.vld.128.ip q3, %[x], 16\n"
        "ee.vmulas.s8.accx.ld.ip q2, %[w], 16, q0, q1\n"
        "ee.vld.128.ip q1, %[x], 16\n"
        "ee.vmulas.s8.accx.ld.ip q0, %[w], 16, q2, q3\n"
        "1:\n"
        "ee.vmulas.s8.accx q0, q1\n"
        "rur.accx_0 %[sum]\n"
        : [sum] "=r"(sum), [w] "+r"(w), [x] "+r"(x)
        : [n] "r"(pairs)
        : "memory");
    return sum;
  }
  pairs = blocks / 2;
  __asm__ volatile(
      "ee.zero.accx\n"
      "ee.vld.128.ip q0, %[w], 16\n"
      "ee.vld.128.ip q1, %[x], 16\n"
      "loopnez %[n], 1f\n"
      "ee.vld.128.ip q3, %[x], 16\n"
      "ee.vmulas.s8.accx.ld.ip q2, %[w], 16, q0, q1\n"
      "ee.vld.128.ip q1, %[x], 16\n"
      "ee.vmulas.s8.accx.ld.ip q0, %[w], 16, q2, q3\n"
      "1:\n"
      "ee.vmulas.s8.accx q0, q1\n"
      "rur.accx_0 %[sum]\n"
      : [sum] "=r"(sum), [w] "+r"(w), [x] "+r"(x)
      : [n] "r"(pairs)
      : "memory");
  return sum;
}

const char *s3_kernel_name(void) { return "esp32s3-simd"; }

/* Bytes of weight rows per PSRAM preload (2, 4 and 8 KB measured within 2%). */
#ifndef S3_PRELOAD_BYTES
#define S3_PRELOAD_BYTES 4096
#endif

static void rows_multi(const int8_t *w, int first, int count, int cols, const int8_t *const *x,
                       int inputs, int32_t *const *y) {
  const int blocks = cols / 16;
  for (int r = first; r < first + count; ++r) {
    const int8_t *row = w + (size_t)r * cols;
    for (int k = 0; k < inputs; ++k)
      y[k][r] = dot_s8(row, x[k], blocks);
  }
}

/* Weights in PSRAM: the data cache preloads the next chunk of rows in the
 * background while the vector unit works on the current one (one manual
 * preload at a time), so memory and arithmetic overlap instead of adding up
 * (kernels 4.4 -> 3.1 ms per frame on en-1). The ROM preload calls are the ones
 * ESP-IDF's own RGB LCD driver uses to stream PSRAM frame buffers. */
static uint32_t preload_autoload;

static void preload(const int8_t *p, int bytes) {
  const uint32_t start = (uint32_t)p & ~63u, end = ((uint32_t)p + bytes + 63u) & ~63u;
  preload_autoload = Cache_Start_DCache_Preload(start, end - start, 0);
}

static void preload_wait(void) {
  while (!Cache_DCache_Preload_Done()) {
  }
  Cache_End_DCache_Preload(preload_autoload);
}

void s3_matvec_multi_s8(const int8_t *w, int rows, int cols, const int8_t *const *x, int inputs,
                        int32_t *const *y) {
  if (!esp_ptr_external_ram(w)) {
    rows_multi(w, 0, rows, cols, x, inputs, y);
    return;
  }
  int chunk = S3_PRELOAD_BYTES / cols;
  if (chunk < 1)
    chunk = 1;
  int n = rows < chunk ? rows : chunk;
  preload(w, n * cols);
  preload_wait();
  for (int r = 0; r < rows;) {
    const int next = r + n, left = rows - next, m = left < chunk ? left : chunk;
    if (m > 0)
      preload(w + (size_t)next * cols, m * cols);
    rows_multi(w, r, n, cols, x, inputs, y);
    if (m > 0)
      preload_wait();
    r = next;
    n = m;
  }
}

#else

static inline int32_t dot_s8(const int8_t *w, const int8_t *x, int blocks) {
  int32_t sum = 0;
  for (int i = 0; i < blocks * 16; ++i)
    sum += (int32_t)w[i] * x[i];
  return sum;
}

const char *s3_kernel_name(void) { return "portable-c"; }

#endif

void s3_matvec_s8(const int8_t *w, int rows, int cols, const int8_t *x, int32_t *y) {
  const int blocks = cols / 16;
  for (int r = 0; r < rows; ++r)
    y[r] = dot_s8(w + (size_t)r * cols, x, blocks);
}

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
void s3_matvec_multi_s8(const int8_t *w, int rows, int cols, const int8_t *const *x, int inputs,
                        int32_t *const *y) {
  /* Every input per row: after the first pass the row is read from the cache. */
  const int blocks = cols / 16;
  for (int r = 0; r < rows; ++r) {
    const int8_t *row = w + (size_t)r * cols;
    for (int k = 0; k < inputs; ++k)
      y[k][r] = dot_s8(row, x[k], blocks);
  }
}
#endif

int32_t s3_dot_s8_reference(const int8_t *w, const int8_t *x, int cols) {
  int32_t sum = 0;
  for (int i = 0; i < cols; ++i)
    sum += (int32_t)w[i] * x[i];
  return sum;
}
