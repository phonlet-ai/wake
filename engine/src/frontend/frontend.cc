#include "frontend.h"

#include <cfenv>
#include <cmath>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "runtime_tables.h"

namespace kws::detail {

namespace {

inline int clamp16(int32_t x) {
  return x < -32768 ? -32768 : x > 32767 ? 32767 : x;
}

// Number of significant bits; the only call site passes v > 0.
inline int clz64(uint64_t v) {
#if defined(_MSC_VER)
  unsigned long index;
  _BitScanReverse64(&index, v);
  return 63 - static_cast<int>(index);
#else
  return __builtin_clzll(v);
#endif
}
inline int bitlen(uint64_t v) { return v ? 64 - clz64(v) : 0; }

// Built once at startup with the identical expression, so every quantized
// value is unchanged. A namespace-scope object, not a function-local static:
// those take a lock on every call on some targets (ESP-IDF __cxa_guard_acquire).
struct QuantLogTable {
  int32_t v[513];
  QuantLogTable() {
    for (int idx = 0; idx <= 512; ++idx)
      v[idx] = (int32_t)std::lround(kQuantDbPerDecade * std::log10((512.0 + idx) / 2048.0));
  }
};
const QuantLogTable quant_log;
} // namespace

const int32_t *quant_log_table() { return quant_log.v; }

Frontend::Frontend() { reset(); }

void Frontend::reset() {
  prev_sample_ = 0.0f;
  for (int i = 0; i < 256; ++i)
    history_[i] = 0.0f;
}

void Frontend::fft512_real_(const float *w) {

  for (int k = 0; k < 256; ++k) {
    int p = kBitrev[k] >> 1;
    re_[k] = w[2 * p];
    im_[k] = w[2 * p + 1];
  }
  // Twiddle-major order (as ESP-DSP's radix-2 kernels): each twiddle is loaded
  // once per stage, not once per butterfly. Butterflies of a stage are
  // independent and each keeps its exact arithmetic, so the result is unchanged.
  for (int m = 1; m <= 128; m <<= 1) {
    const int span = 2 * m, step = 256 / m;
    for (int j = 0; j < m; ++j) {
      const float c = kSinTable[j * step + 128];
      const float s = kSinTable[j * step];
      float *lo_re = re_ + j, *lo_im = im_ + j;
      for (int base = 0; base < 256; base += span) {
        float lr = lo_re[base], li = lo_im[base];
        float hr = lo_re[base + m], hi = lo_im[base + m];
        float t_re = hr * c + s * hi;
        float t_im = hi * c - s * hr;
        lo_re[base] = lr + t_re;
        lo_im[base] = li + t_im;
        lo_re[base + m] = lr - t_re;
        lo_im[base + m] = li - t_im;
      }
    }
  }
  const float inv = 0.001953125f;
  const float inv2 = kFftSplitScale;
  float z0r = re_[0], z0i = im_[0];
  float dc = (z0r + z0i) * inv;
  power_[0] = dc * dc;
  for (int k = 1; k < 256; ++k) {
    float a_re = re_[k], a_im = im_[k];
    float b_re = re_[256 - k], b_im = im_[256 - k];
    float sum_im = a_im + b_im;
    float dif_re = b_re - a_re;
    float sc = kSinTable[k + 128], ss = kSinTable[k];
    float orr = ((b_re + a_re) + sc * sum_im + ss * dif_re) * inv2;
    float oii = ((a_im - b_im) + sc * dif_re - ss * sum_im) * inv2;
    power_[k] = oii * oii + orr * orr;
  }
  float nr = (z0r - z0i) * inv;
  power_[256] = nr * nr;
}

void Frontend::process(const int16_t *pcm256, int16_t mel40[40]) {

  float pre[256];
  pre[0] = (float)pcm256[0] - kPreemph * prev_sample_;
  for (int i = 1; i < 256; ++i)
    pre[i] = (float)pcm256[i] - kPreemph * (float)pcm256[i - 1];
  prev_sample_ = (float)pcm256[255];

  for (int i = 0; i < 256; ++i)
    frame_[i] = history_[i];
  for (int i = 0; i < 256; ++i)
    frame_[256 + i] = pre[i];
  for (int i = 0; i < 256; ++i)
    history_[i] = pre[i];

  for (int i = 0; i < 256; ++i)
    frame_[i] *= kFftWindow[i];
  for (int i = 0; i < 256; ++i)
    frame_[256 + i] *= kFftWindow[255 - i];

  fft512_real_(frame_);

  for (int b = 0; b < 40; ++b) {
    int s = kMelEdges[b], c = kMelEdges[b + 1], e = kMelEdges[b + 2];

    float acc = 0.0f, wgt = 0.0f;
    int c_end = c < 257 ? c : 257;
    for (int k = s; k < c_end; ++k) {
      acc += power_[k] * wgt;
      wgt = wgt + kMelRising[b];
    }
    int e_end = e < 257 ? e : 257;
    for (int k = c; k < e_end; ++k) {
      acc += power_[k] * wgt;
      wgt = wgt - kMelFalling[b];
    }

    float f = acc * kMelEnergyScale;

    {
      int64_t v;
      bool big = f >= 9223372036854775808.0f;
      v = big ? 0x3fffffffffffffff : (int64_t)f;
      int32_t out_r = kSilenceValue;
      if (v != 0) {
        int n = big ? -1 : 62 - bitlen((uint64_t)v);
        uint64_t vn = n >= 0 ? ((uint64_t)v << n) : ((uint64_t)v >> -n);
        if (big)
          vn = 0x3fffffffffffffff;
        int idx = (int)((vn + (1ULL << 51)) >> 52) - 512;
        if (idx < 0)
          idx = 0;
        if (idx > 512)
          idx = 512;
        int32_t res = quant_log_table()[idx] - (int32_t)kQuantOctaveStep * n;
        out_r = clamp16(res);
      }
      mel40[b] = (int16_t)out_r;
    }
  }
}

} // namespace kws::detail
