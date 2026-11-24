#include "float_model.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace kws::detail {

namespace {

// IEEE half -> float, exact (normals, subnormals, inf, nan).
inline float half_to_float(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1f;
  uint32_t mant = h & 0x3ff;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      int e = -1;
      do {
        ++e;
        mant <<= 1;
      } while (!(mant & 0x400));
      bits = sign | (uint32_t)(112 - e) << 23 | (mant & 0x3ff) << 13;
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000u | mant << 13;
  } else {
    bits = sign | (exp + 112) << 23 | mant << 13;
  }
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

// Fixed reduction of eight partial sums plus the sequential tail.
inline float reduce8(const float s[8], float tail) {
  float a0 = s[0] + s[4], a1 = s[1] + s[5], a2 = s[2] + s[6], a3 = s[3] + s[7];
  return ((a0 + a2) + (a1 + a3)) + tail;
}

template <class Load>
float dot_portable(const float *x, int n, Load load) {
  float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int j = 0;
  for (; j + 8 <= n; j += 8)
    for (int k = 0; k < 8; ++k)
      s[k] = std::fmaf(x[j + k], load(j + k), s[k]);
  float tail = 0;
  for (; j < n; ++j)
    tail = std::fmaf(x[j], load(j), tail);
  return reduce8(s, tail);
}

using RowDot = float (*)(const float *x, const void *w, int n);

float row_f32_portable(const float *x, const void *w, int n) {
  auto p = static_cast<const float *>(w);
  return dot_portable(x, n, [p](int j) { return p[j]; });
}
float row_f16_portable(const float *x, const void *w, int n) {
  auto p = static_cast<const uint16_t *>(w);
  return dot_portable(x, n, [p](int j) { return half_to_float(p[j]); });
}
float row_i8_portable(const float *x, const void *w, int n) {
  auto p = static_cast<const int8_t *>(w);
  return dot_portable(x, n, [p](int j) { return (float)p[j]; });
}

#if defined(__x86_64__) || defined(__i386__)
// fused multiply-add per lane: the same rounding as the portable fmaf loop,
// lane k of the vector accumulator is partial sum k.
__attribute__((target("avx2,fma"))) inline float finish(__m256 acc, const float *x,
                                                     int j, int n, float tail_w(const void *, int),
                                                     const void *w) {
  alignas(32) float s[8];
  _mm256_store_ps(s, acc);
  float tail = 0;
  for (; j < n; ++j)
    tail = std::fmaf(x[j], tail_w(w, j), tail);
  return reduce8(s, tail);
}
float tail_f32(const void *w, int j) { return static_cast<const float *>(w)[j]; }
float tail_f16(const void *w, int j) {
  return half_to_float(static_cast<const uint16_t *>(w)[j]);
}
float tail_i8(const void *w, int j) { return (float)static_cast<const int8_t *>(w)[j]; }

__attribute__((target("avx2,fma"))) float row_f32_avx2(const float *x, const void *w, int n) {
  auto p = static_cast<const float *>(w);
  __m256 acc = _mm256_setzero_ps();
  int j = 0;
  for (; j + 8 <= n; j += 8)
    acc = _mm256_fmadd_ps(_mm256_loadu_ps(x + j), _mm256_loadu_ps(p + j), acc);
  return finish(acc, x, j, n, tail_f32, w);
}
__attribute__((target("avx2,f16c,fma"))) float row_f16_avx2(const float *x, const void *w, int n) {
  auto p = static_cast<const uint16_t *>(w);
  __m256 acc = _mm256_setzero_ps();
  int j = 0;
  for (; j + 8 <= n; j += 8) {
    __m256 wv = _mm256_cvtph_ps(_mm_loadu_si128(reinterpret_cast<const __m128i *>(p + j)));
    acc = _mm256_fmadd_ps(_mm256_loadu_ps(x + j), wv, acc);
  }
  return finish(acc, x, j, n, tail_f16, w);
}
__attribute__((target("avx2,fma"))) float row_i8_avx2(const float *x, const void *w, int n) {
  auto p = static_cast<const int8_t *>(w);
  __m256 acc = _mm256_setzero_ps();
  int j = 0;
  for (; j + 8 <= n; j += 8) {
    __m128i b = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(p + j));
    __m256 wv = _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(b));
    acc = _mm256_fmadd_ps(_mm256_loadu_ps(x + j), wv, acc);
  }
  return finish(acc, x, j, n, tail_i8, w);
}
#endif

struct Kernels {
  RowDot f32, f16, i8;
  const char *name;
};

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
static bool float_f16c_supported() {
#if defined(__clang__)
  // clang rejects the "f16c" feature string; read CPUID leaf 1, ECX bit 29.
  unsigned eax, ebx, ecx, edx;
  return __get_cpuid_count(1, 0, &eax, &ebx, &ecx, &edx) &&
         (ecx & (1u << 29)) != 0;
#else
  return __builtin_cpu_supports("f16c");
#endif
}
#endif

Kernels choose() {
  const char *env = std::getenv("KWS_FLOAT_KERNEL");
  std::string want = env ? env : "auto";
  Kernels portable{row_f32_portable, row_f16_portable, row_i8_portable, "portable"};
  if (want == "portable")
    return portable;
  if (want != "auto" && want != "avx2")
    throw std::runtime_error("KWS_FLOAT_KERNEL must be auto, portable or avx2");
#if defined(__x86_64__) || defined(__i386__)
  if (__builtin_cpu_supports("avx2") && float_f16c_supported() &&
      __builtin_cpu_supports("fma"))
    return {row_f32_avx2, row_f16_avx2, row_i8_avx2, "avx2"};
#endif
  if (want == "avx2")
    throw std::runtime_error("AVX2/F16C/FMA unavailable on this CPU");
  return portable;
}

const Kernels &kernels() {
  static const Kernels k = choose();
  return k;
}

} // namespace

const char *float_kernel() { return kernels().name; }

size_t FloatMatrix::bytes() const {
  return f32.size() * 4 + f16.size() * 2 + i8.size() + scale.size() * 4;
}

void matvec(const FloatMatrix &m, const float *x, float *y) {
  const auto &k = kernels();
  switch (m.storage) {
  case Storage::f32:
    for (int r = 0; r < m.rows; ++r)
      y[r] = k.f32(x, m.f32.data() + (size_t)r * m.cols, m.cols);
    break;
  case Storage::f16:
    for (int r = 0; r < m.rows; ++r)
      y[r] = k.f16(x, m.f16.data() + (size_t)r * m.cols, m.cols);
    break;
  case Storage::i8:
    for (int r = 0; r < m.rows; ++r)
      y[r] = k.i8(x, m.i8.data() + (size_t)r * m.cols, m.cols) * m.scale[r];
    break;
  }
}

#if defined(__x86_64__) || defined(__i386__)
// 4 rows x 2 vectors of 8-lane FMA accumulators. Each output's lane k sums
// x[j+k]*w[j+k] over the 8-blocks in order, then the same tail and reduce8
// as row_f32_avx2: bit-identical to the single-dot kernel, with each weight
// block loaded once for both vectors and the reductions amortised.
__attribute__((target("avx2,fma"))) static void tile_f32_4x2(const float *const w[4],
                                                              const float *x0, const float *x1,
                                                              int n, float out[4][2]) {
  __m256 a00 = _mm256_setzero_ps(), a01 = a00, a10 = a00, a11 = a00, a20 = a00, a21 = a00,
         a30 = a00, a31 = a00;
  int j = 0;
  for (; j + 8 <= n; j += 8) {
    __m256 v0 = _mm256_loadu_ps(x0 + j), v1 = _mm256_loadu_ps(x1 + j);
    __m256 w0 = _mm256_loadu_ps(w[0] + j);
    a00 = _mm256_fmadd_ps(v0, w0, a00);
    a01 = _mm256_fmadd_ps(v1, w0, a01);
    __m256 w1 = _mm256_loadu_ps(w[1] + j);
    a10 = _mm256_fmadd_ps(v0, w1, a10);
    a11 = _mm256_fmadd_ps(v1, w1, a11);
    __m256 w2 = _mm256_loadu_ps(w[2] + j);
    a20 = _mm256_fmadd_ps(v0, w2, a20);
    a21 = _mm256_fmadd_ps(v1, w2, a21);
    __m256 w3 = _mm256_loadu_ps(w[3] + j);
    a30 = _mm256_fmadd_ps(v0, w3, a30);
    a31 = _mm256_fmadd_ps(v1, w3, a31);
  }
  __m256 acc[4][2] = {{a00, a01}, {a10, a11}, {a20, a21}, {a30, a31}};
  const float *xs[2] = {x0, x1};
  for (int r = 0; r < 4; ++r)
    for (int s = 0; s < 2; ++s)
      out[r][s] = finish(acc[r][s], xs[s], j, n, tail_f32, w[r]);
}
#endif

void matvec_batch(const FloatMatrix &m, const float *const *x, float *const *y, int n) {
  const auto &k = kernels();
#if defined(__x86_64__) || defined(__i386__)
  if (m.storage == Storage::f32 && k.f32 == row_f32_avx2 && n >= 2) {
    int r = 0;
    for (; r + 4 <= m.rows; r += 4) {
      const float *w[4] = {m.f32.data() + (size_t)r * m.cols, m.f32.data() + (size_t)(r + 1) * m.cols,
                           m.f32.data() + (size_t)(r + 2) * m.cols,
                           m.f32.data() + (size_t)(r + 3) * m.cols};
      int s = 0;
      for (; s + 2 <= n; s += 2) {
        float out[4][2];
        tile_f32_4x2(w, x[s], x[s + 1], m.cols, out);
        for (int i = 0; i < 4; ++i) {
          y[s][r + i] = out[i][0];
          y[s + 1][r + i] = out[i][1];
        }
      }
      for (; s < n; ++s)
        for (int i = 0; i < 4; ++i)
          y[s][r + i] = k.f32(x[s], w[i], m.cols);
    }
    for (; r < m.rows; ++r)
      for (int s = 0; s < n; ++s)
        y[s][r] = k.f32(x[s], m.f32.data() + (size_t)r * m.cols, m.cols);
    return;
  }
#endif
  for (int r = 0; r < m.rows; ++r) {
    switch (m.storage) {
    case Storage::f32: {
      const float *w = m.f32.data() + (size_t)r * m.cols;
      for (int s = 0; s < n; ++s)
        y[s][r] = k.f32(x[s], w, m.cols);
      break;
    }
    case Storage::f16: {
      const uint16_t *w = m.f16.data() + (size_t)r * m.cols;
      for (int s = 0; s < n; ++s)
        y[s][r] = k.f16(x[s], w, m.cols);
      break;
    }
    case Storage::i8: {
      const int8_t *w = m.i8.data() + (size_t)r * m.cols;
      for (int s = 0; s < n; ++s)
        y[s][r] = k.i8(x[s], w, m.cols) * m.scale[r];
      break;
    }
    }
  }
}

size_t FloatModelFile::parameters() const {
  size_t n = affine_offset.size() + affine_scale.size() + head.parameters() + head_bias.size() +
             final_norm.size() + embed.parameters() + embed_b.size();
  for (const auto &l : layers) {
    n += l.b_ih.size() + l.b_hh.size() + l.norm.size() + l.mlp_norm.size() + l.up_b.size() +
         l.down_b.size() + l.up.parameters() + l.down.parameters() + l.out.parameters() + l.out_b.size();
    n += l.kind == LayerKind::joint ? l.u.parameters()
         : input_gated(l.kind)      ? l.x.parameters()
                                    : l.x.parameters() + l.h.parameters();
  }
  return n;
}

size_t FloatModelFile::tensor_bytes() const {
  auto op = [](const FloatOp &o) { return o.lowrank ? o.a.bytes() + o.b.bytes() : o.w.bytes(); };
  size_t n = 4 * (affine_offset.size() + affine_scale.size() + head_bias.size() + final_norm.size() +
                  embed_b.size()) + op(head) + (published ? op(embed) : 0);
  for (const auto &l : layers) {
    n += 4 * (l.b_ih.size() + l.b_hh.size() + l.norm.size() + l.mlp_norm.size() + l.up_b.size() +
              l.down_b.size() + l.out_b.size());
    if (published)
      n += op(l.up) + op(l.down) + (out_proj ? op(l.out) : 0);
    n += l.kind == LayerKind::joint ? op(l.u) : input_gated(l.kind) ? op(l.x) : op(l.x) + op(l.h);
  }
  return n;
}

FloatModel::FloatModel(const FloatModelFile &file) : file_(file) {
  const int H = file_.hidden;
  int widest = 80, rank = 1;
  for (const auto &l : file_.layers) {
    widest = std::max(widest, l.inputs + H);
    for (const FloatOp *o : {&l.u, &l.x, &l.h})
      rank = std::max(rank, o->rank);
  }
  x_.assign(80, 0);
  h_.assign((size_t)file_.n_layers * H, 0);
  c_.assign((size_t)file_.n_layers * H, 0);
  a_.assign((size_t)3 * H, 0);
  tmp_.assign((size_t)3 * H, 0);
  r_.assign(H, 0);
  int mlp = 1;
  for (const auto &l : file_.layers)
    mlp = std::max(mlp, l.up.rows);
  m_.assign(mlp, 0);
  v_.assign(widest, 0);
  z_.assign(std::max(rank, file_.head.rank), 0);
  logits_.assign(file_.n_phones, 0);
  reset();
}

void FloatModel::reset() {
  std::fill(h_.begin(), h_.end(), 0.f);
  std::fill(c_.begin(), c_.end(), 0.f);
}

void FloatModel::apply(const FloatOp &op, const float *v, float *y) {
  if (!op.lowrank) {
    matvec(op.w, v, y);
    return;
  }
  matvec(op.b, v, z_.data());
  matvec(op.a, z_.data(), y);
}

namespace {

void features(const FloatModelFile &f, const int16_t *mel80, float *x) {
  for (int i = 0; i < 80; ++i) {
    int k = i < 40 ? i : i - 40;
    float m = (float)mel80[i] / 1024.0f;
    x[i] = (m - f.affine_offset[k]) * f.affine_scale[k];
  }
}

void joint_bias(const FloatLayer &layer, float *a, int rows) {
  for (int r = 0; r < rows; ++r)
    a[r] = (a[r] + layer.b_ih[r]) + layer.b_hh[r];
}

void split_bias(const FloatLayer &layer, float *a, const float *t, int rows) {
  for (int r = 0; r < rows; ++r)
    a[r] = (a[r] + layer.b_ih[r]) + (t[r] + layer.b_hh[r]);
}

void cell(const float *a, float *c, float *h, int H) {
  for (int i = 0; i < H; ++i) {
    float ig = 1.0f / (1.0f + std::exp(-a[i]));
    float g = std::tanh(a[H + i]);
    float og = 1.0f / (1.0f + std::exp(-a[2 * H + i]));
    float cn = (1.0f - ig) * c[i] + ig * g;
    c[i] = cn;
    h[i] = og * std::tanh(cn);
  }
}

float sigmoid(float v) { return 1.0f / (1.0f + std::exp(-v)); }

// log(sigmoid(v)) without overflow, as torch's logsigmoid
float log_sigmoid(float v) { return std::min(v, 0.0f) - std::log1p(std::exp(-std::fabs(v))); }

// One input-gated layer step: a = W x + b (G*H rows) -> state s, output h.
// candidate: 0 linear, 1 g, 2 tanh (minGRU's h~)
void gated_cell(LayerKind kind, Mixer mixer, int candidate, const float *a, float *s, float *h,
                int H) {
  for (int i = 0; i < H; ++i) {
    float out;
    if (kind == LayerKind::mingru) {
      float z = a[i], ht = a[H + i];
      if (candidate == 1)
        ht = ht >= 0 ? ht + 0.5f : sigmoid(ht);
      else if (candidate == 2)
        ht = std::tanh(ht);
      s[i] = std::exp(log_sigmoid(-z)) * s[i] + sigmoid(z) * ht;
      out = s[i];
    } else if (kind == LayerKind::igcifg) {
      float ig = a[i];
      s[i] = std::exp(log_sigmoid(-ig)) * s[i] + sigmoid(ig) * std::tanh(a[H + i]);
      out = sigmoid(a[2 * H + i]) * std::tanh(s[i]);
    } else { // minlstm
      // f' = sigmoid(-d), i' = sigmoid(d), d = log sigmoid(i) - log sigmoid(f): the
      // normalised pair sums to one after rounding
      float d = log_sigmoid(a[H + i]) - log_sigmoid(a[i]);
      s[i] = sigmoid(-d) * s[i] + sigmoid(d) * a[2 * H + i];
      out = s[i];
    }
    if (mixer == Mixer::tanh)
      out = std::tanh(out);
    else if (mixer == Mixer::silu)
      out = out * sigmoid(out);
    h[i] = out;
  }
}

// y = x / sqrt(mean(x^2) + 1e-5) * gain (models.RMSNorm)
void rmsnorm(const float *x, const float *gain, float *y, int H) {
  double ss = 0;
  for (int i = 0; i < H; ++i)
    ss += (double)x[i] * x[i];
  float rs = (float)(1.0 / std::sqrt(ss / H + 1e-5));
  for (int i = 0; i < H; ++i)
    y[i] = x[i] * rs * gain[i];
}

int candidate(const FloatModelFile &f) { return f.candidate_g ? 1 : f.candidate_tanh ? 2 : 0; }

void add_bias(float *y, const std::vector<float> &b) {
  for (size_t i = 0; i < b.size(); ++i)
    y[i] += b[i];
}

void silu(float *y, int n) {
  for (int i = 0; i < n; ++i)
    y[i] = y[i] * sigmoid(y[i]);
}

int widest_mlp(const FloatModelFile &f) {
  int e = 1;
  for (const auto &l : f.layers)
    e = std::max(e, l.up.rows);
  return e;
}

void gated_bias(const FloatLayer &layer, float *a, int rows) {
  for (int r = 0; r < rows; ++r)
    a[r] += layer.b_ih[r];
}

void softmax_q31(const FloatModelFile &f, float *logits, std::vector<double> &e, int32_t *post) {
  double mx = -1e300;
  for (int i = 0; i < f.n_phones; ++i) {
    logits[i] += f.head_bias[i];
    mx = std::max(mx, (double)logits[i]);
  }
  double sum = 0;
  e.resize(f.n_phones);
  for (int i = 0; i < f.n_phones; ++i) {
    e[i] = std::exp((double)logits[i] - mx);
    sum += e[i];
  }
  for (int i = 0; i < f.n_phones; ++i) {
    double q = std::nearbyint(e[i] / sum * 2147483648.0);
    post[i] = (int32_t)(q < 0 ? 0 : q > 2147483647.0 ? 2147483647.0 : q);
  }
}

int widest_input(const FloatModelFile &f) {
  int w = 80;
  for (const auto &l : f.layers)
    w = std::max(w, l.inputs + f.hidden);
  return w;
}

int widest_rank(const FloatModelFile &f) {
  int r = std::max(1, f.head.rank);
  for (const auto &l : f.layers)
    for (const FloatOp *o : {&l.u, &l.x, &l.h})
      r = std::max(r, o->rank);
  return r;
}

} // namespace

void FloatModel::step(const int16_t *mel80, int32_t *post) {
  const int H = file_.hidden;
  features(file_, mel80, x_.data());
  if (file_.published) {
    float *r = r_.data(), *n = tmp_.data(), *a = a_.data(), *m = m_.data();
    apply(file_.embed, x_.data(), r);
    add_bias(r, file_.embed_b);
    for (int l = 0; l < file_.n_layers; ++l) {
      const auto &layer = file_.layers[l];
      float *h = h_.data() + (size_t)l * H, *c = c_.data() + (size_t)l * H;
      rmsnorm(r, layer.norm.data(), n, H);
      apply(layer.x, n, a);
      gated_bias(layer, a, gate_rows(layer.kind) * H);
      gated_cell(layer.kind, Mixer::none, candidate(file_), a, c, h, H);
      if (file_.out_proj) {
        apply(layer.out, h, n);
        add_bias(n, layer.out_b);
        for (int i = 0; i < H; ++i)
          r[i] += n[i];
      } else {
        for (int i = 0; i < H; ++i)
          r[i] += h[i];
      }
      rmsnorm(r, layer.mlp_norm.data(), n, H);
      apply(layer.up, n, m);
      add_bias(m, layer.up_b);
      silu(m, layer.up.rows);
      apply(layer.down, m, a);
      add_bias(a, layer.down_b);
      for (int i = 0; i < H; ++i)
        r[i] += a[i];
    }
    rmsnorm(r, file_.final_norm.data(), n, H);
    apply(file_.head, n, logits_.data());
    softmax_q31(file_, logits_.data(), e_, post);
    return;
  }
  for (int l = 0; l < file_.n_layers; ++l) {
    const auto &layer = file_.layers[l];
    const float *in = l == 0 ? x_.data() : h_.data() + (size_t)(l - 1) * H;
    float *h = h_.data() + (size_t)l * H;
    float *c = c_.data() + (size_t)l * H;
    float *a = a_.data();
    if (input_gated(layer.kind)) {
      if (file_.residual && l) {
        rmsnorm(r_.data(), layer.norm.data(), tmp_.data(), H);
        in = tmp_.data();
      }
      apply(layer.x, in, a);
      gated_bias(layer, a, gate_rows(layer.kind) * H);
      gated_cell(layer.kind, file_.mixer, candidate(file_), a, c, h, H);
      if (file_.residual)
        for (int i = 0; i < H; ++i)
          r_[i] = l ? r_[i] + h[i] : h[i];
      continue;
    }
    if (layer.kind == LayerKind::joint) {
      std::copy(in, in + layer.inputs, v_.begin());
      std::copy(h, h + H, v_.begin() + layer.inputs);
      apply(layer.u, v_.data(), a);
      joint_bias(layer, a, 3 * H);
    } else {
      apply(layer.x, in, a);
      apply(layer.h, h, tmp_.data());
      split_bias(layer, a, tmp_.data(), 3 * H);
    }
    cell(a, c, h, H);
  }
  const float *top = h_.data() + (size_t)(file_.n_layers - 1) * H;
  if (file_.residual) {
    rmsnorm(r_.data(), file_.final_norm.data(), tmp_.data(), H);
    top = tmp_.data();
  }
  apply(file_.head, top, logits_.data());
  softmax_q31(file_, logits_.data(), e_, post);
}

// ------------------------------------------------------------ FloatBatch

FloatBatch::FloatBatch(const FloatModelFile &file, int streams) : file_(file), K_(streams) {
  const int H = file_.hidden;
  auto make = [&](size_t n) { return std::vector<std::vector<float>>(K_, std::vector<float>(n, 0.f)); };
  x_ = make(80);
  h_ = make((size_t)file_.n_layers * H);
  c_ = make((size_t)file_.n_layers * H);
  a_ = make((size_t)3 * H);
  tmp_ = make((size_t)3 * H);
  v_ = make(widest_input(file_));
  z_ = make(widest_rank(file_));
  logits_ = make(file_.n_phones);
  r_ = make(H);
  m_ = make(widest_mlp(file_));
  in_.resize(K_);
  out_.resize(K_);
  out2_.resize(K_);
  idx_.resize(K_);
}

void FloatBatch::reset(int s) {
  std::fill(h_[s].begin(), h_[s].end(), 0.f);
  std::fill(c_[s].begin(), c_[s].end(), 0.f);
}

void FloatBatch::apply(const FloatOp &op, int n, const float *const *v, float *const *y) {
  if (!op.lowrank) {
    matvec_batch(op.w, v, y, n);
    return;
  }
  std::vector<float *> z(n);
  for (int j = 0; j < n; ++j)
    z[j] = z_[idx_[j]].data();
  matvec_batch(op.b, v, z.data(), n);
  matvec_batch(op.a, z.data(), y, n);
}

void FloatBatch::step(const int16_t *const *mel, int32_t *const *post, const uint8_t *active) {
  const int H = file_.hidden;
  int n = 0;
  for (int s = 0; s < K_; ++s)
    if (active[s])
      idx_[n++] = s;
  if (!n)
    return;
  for (int j = 0; j < n; ++j)
    features(file_, mel[idx_[j]], x_[idx_[j]].data());
  if (file_.published) {
    auto point = [&](std::vector<std::vector<float>> &buf, std::vector<const float *> *cin,
                     std::vector<float *> *out) {
      for (int j = 0; j < n; ++j) {
        if (cin)
          (*cin)[j] = buf[idx_[j]].data();
        if (out)
          (*out)[j] = buf[idx_[j]].data();
      }
    };
    point(x_, &in_, nullptr);
    point(r_, nullptr, &out_);
    apply(file_.embed, n, in_.data(), out_.data());
    for (int j = 0; j < n; ++j)
      add_bias(r_[idx_[j]].data(), file_.embed_b);
    for (int l = 0; l < file_.n_layers; ++l) {
      const auto &layer = file_.layers[l];
      for (int j = 0; j < n; ++j) {
        int s = idx_[j];
        rmsnorm(r_[s].data(), layer.norm.data(), tmp_[s].data(), H);
      }
      point(tmp_, &in_, nullptr);
      point(a_, nullptr, &out_);
      apply(layer.x, n, in_.data(), out_.data());
      for (int j = 0; j < n; ++j) {
        int s = idx_[j];
        float *h = h_[s].data() + (size_t)l * H;
        gated_bias(layer, a_[s].data(), gate_rows(layer.kind) * H);
        gated_cell(layer.kind, Mixer::none, candidate(file_), a_[s].data(), c_[s].data() + (size_t)l * H, h, H);
        if (!file_.out_proj)
          for (int i = 0; i < H; ++i)
            r_[s][i] += h[i];
      }
      if (file_.out_proj) {
        for (int j = 0; j < n; ++j)
          in_[j] = h_[idx_[j]].data() + (size_t)l * H;
        point(tmp_, nullptr, &out_);
        apply(layer.out, n, in_.data(), out_.data());
        for (int j = 0; j < n; ++j) {
          int s = idx_[j];
          add_bias(tmp_[s].data(), layer.out_b);
          for (int i = 0; i < H; ++i)
            r_[s][i] += tmp_[s][i];
        }
      }
      for (int j = 0; j < n; ++j) {
        int s = idx_[j];
        rmsnorm(r_[s].data(), layer.mlp_norm.data(), tmp_[s].data(), H);
      }
      point(tmp_, &in_, nullptr);
      point(m_, nullptr, &out_);
      apply(layer.up, n, in_.data(), out_.data());
      for (int j = 0; j < n; ++j) {
        add_bias(m_[idx_[j]].data(), layer.up_b);
        silu(m_[idx_[j]].data(), layer.up.rows);
      }
      point(m_, &in_, nullptr);
      point(a_, nullptr, &out_);
      apply(layer.down, n, in_.data(), out_.data());
      for (int j = 0; j < n; ++j) {
        int s = idx_[j];
        add_bias(a_[s].data(), layer.down_b);
        for (int i = 0; i < H; ++i)
          r_[s][i] += a_[s][i];
      }
    }
    for (int j = 0; j < n; ++j) {
      int s = idx_[j];
      rmsnorm(r_[s].data(), file_.final_norm.data(), tmp_[s].data(), H);
    }
    point(tmp_, &in_, nullptr);
    point(logits_, nullptr, &out_);
    apply(file_.head, n, in_.data(), out_.data());
    for (int j = 0; j < n; ++j)
      softmax_q31(file_, logits_[idx_[j]].data(), e_, post[idx_[j]]);
    return;
  }
  for (int l = 0; l < file_.n_layers; ++l) {
    const auto &layer = file_.layers[l];
    for (int j = 0; j < n; ++j) {
      int s = idx_[j];
      out_[j] = a_[s].data();
      out2_[j] = tmp_[s].data();
    }
    if (input_gated(layer.kind)) {
      for (int j = 0; j < n; ++j) {
        int s = idx_[j];
        if (file_.residual && l) {
          rmsnorm(r_[s].data(), layer.norm.data(), tmp_[s].data(), H);
          in_[j] = tmp_[s].data();
        } else {
          in_[j] = l == 0 ? x_[s].data() : h_[s].data() + (size_t)(l - 1) * H;
        }
      }
      apply(layer.x, n, in_.data(), out_.data());
      for (int j = 0; j < n; ++j) {
        int s = idx_[j];
        float *h = h_[s].data() + (size_t)l * H;
        gated_bias(layer, out_[j], gate_rows(layer.kind) * H);
        gated_cell(layer.kind, file_.mixer, candidate(file_), out_[j], c_[s].data() + (size_t)l * H, h, H);
        if (file_.residual)
          for (int i = 0; i < H; ++i)
            r_[s][i] = l ? r_[s][i] + h[i] : h[i];
      }
      continue;
    }
    if (layer.kind == LayerKind::joint) {
      for (int j = 0; j < n; ++j) {
        int s = idx_[j];
        const float *in = l == 0 ? x_[s].data() : h_[s].data() + (size_t)(l - 1) * H;
        const float *h = h_[s].data() + (size_t)l * H;
        std::copy(in, in + layer.inputs, v_[s].begin());
        std::copy(h, h + H, v_[s].begin() + layer.inputs);
        in_[j] = v_[s].data();
      }
      apply(layer.u, n, in_.data(), out_.data());
      for (int j = 0; j < n; ++j)
        joint_bias(layer, out_[j], 3 * H);
    } else {
      for (int j = 0; j < n; ++j) {
        int s = idx_[j];
        in_[j] = l == 0 ? x_[s].data() : h_[s].data() + (size_t)(l - 1) * H;
      }
      apply(layer.x, n, in_.data(), out_.data());
      for (int j = 0; j < n; ++j)
        in_[j] = h_[idx_[j]].data() + (size_t)l * H;
      apply(layer.h, n, in_.data(), out2_.data());
      for (int j = 0; j < n; ++j)
        split_bias(layer, out_[j], out2_[j], 3 * H);
    }
    for (int j = 0; j < n; ++j) {
      int s = idx_[j];
      cell(a_[s].data(), c_[s].data() + (size_t)l * H, h_[s].data() + (size_t)l * H, H);
    }
  }
  for (int j = 0; j < n; ++j) {
    int s = idx_[j];
    in_[j] = h_[s].data() + (size_t)(file_.n_layers - 1) * H;
    if (file_.residual) {
      rmsnorm(r_[s].data(), file_.final_norm.data(), tmp_[s].data(), H);
      in_[j] = tmp_[s].data();
    }
    out_[j] = logits_[s].data();
  }
  apply(file_.head, n, in_.data(), out_.data());
  for (int j = 0; j < n; ++j)
    softmax_q31(file_, logits_[idx_[j]].data(), e_, post[idx_[j]]);
}

} // namespace kws::detail
