#include "kernels.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif
namespace kws::detail {
int32_t dot_portable(const int16_t *x, const int16_t *w, int n) {
  int32_t sum = 0;
  for (int j = 0; j < n; ++j)
    sum += int32_t(x[j]) * w[j];
  return sum;
}
#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2"))) static int32_t
dot_avx2(const int16_t *x, const int16_t *w, int n) {
  __m256i sum = _mm256_setzero_si256();
  int j = 0;
  for (; j + 16 <= n; j += 16) {
    auto a = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(x + j));
    auto b = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(w + j));
    sum = _mm256_add_epi32(sum, _mm256_madd_epi16(a, b));
  }
  alignas(32) int32_t lanes[8];
  _mm256_store_si256(reinterpret_cast<__m256i *>(lanes), sum);
  int32_t out = 0;
  for (auto v : lanes)
    out += v;
  for (; j < n; ++j)
    out += int32_t(x[j]) * w[j];
  return out;
}
#endif
#if defined(__wasm_simd128__)
// i32x4_dot_i16x8 is the exact analogue of _mm256_madd_epi16: each lane holds
// the sum of two int16 products, so the partial sums differ from the portable
// loop only in grouping. int32 addition wraps identically whatever the
// grouping, so the result is bit-identical even where the accumulator
// overflows.
static int32_t dot_simd128(const int16_t *x, const int16_t *w, int n) {
  v128_t sum = wasm_i32x4_splat(0);
  int j = 0;
  for (; j + 8 <= n; j += 8) {
    v128_t a = wasm_v128_load(x + j);
    v128_t b = wasm_v128_load(w + j);
    sum = wasm_i32x4_add(sum, wasm_i32x4_dot_i16x8(a, b));
  }
  int32_t out = wasm_i32x4_extract_lane(sum, 0) + wasm_i32x4_extract_lane(sum, 1) +
                wasm_i32x4_extract_lane(sum, 2) + wasm_i32x4_extract_lane(sum, 3);
  for (; j < n; ++j)
    out += int32_t(x[j]) * w[j];
  return out;
}
#endif

struct Selection {
  Dot dot;
  const char *name;
};
static Selection select() {
  const char *choice = std::getenv("KWS_CPU_KERNEL");
  if (!choice || !strcmp(choice, "portable"))
    return {dot_portable, "portable"};
  if (strcmp(choice, "auto") && strcmp(choice, "avx2") && strcmp(choice, "simd128"))
    throw std::runtime_error(
        "KWS_CPU_KERNEL must be portable, auto, avx2 or simd128");
#if defined(__wasm_simd128__)
  if (strcmp(choice, "avx2"))
    return {dot_simd128, "simd128"};
#endif
  if (!strcmp(choice, "simd128"))
    throw std::runtime_error("simd128 is a WebAssembly kernel");
#if defined(__x86_64__) || defined(__i386__)
  if (__builtin_cpu_supports("avx2"))
    return {dot_avx2, "avx2"};
#endif
  if (!strcmp(choice, "avx2"))
    throw std::runtime_error("AVX2 is unavailable on this CPU");
  return {dot_portable, "portable"};
}
static const Selection &selection() {
  static const Selection value = select();
  return value;
}
Dot selected_dot() { return selection().dot; }
const char *selected_kernel() { return selection().name; }
} // namespace kws::detail
