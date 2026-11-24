// ESP32-S3 acoustic engine for Phonlet Wake.
//
// Runs the published minGRU stack of a float container (en-1: six layers,
// width 162, int8 weights with one scale per row) with integer matrix
// products on the S3 vector unit instead of fp32 fused multiply-adds:
//
//   * weights stay int8, repacked once into zero-padded 16-byte rows, placed
//     in internal SRAM while it lasts and in PSRAM after that;
//   * each matrix input is quantised per vector to 16 bits and split into two
//     signed 8-bit halves (q = 256 hi + lo + 128), so the 8-bit SIMD keeps
//     about 15 bits of activation precision; 8-bit activations (one pass) can
//     be chosen instead;
//   * norms, gates, the recurrence and the softmax stay fp32 (a few thousand
//     operations per frame); sigmoid and tanh come from an interpolated table.
//
// The integer sums are exact, so the portable C build of this file is the
// bit-exact reference of the firmware. Its posteriors differ from FloatModel's
// by the activation quantisation only; the host comparison in
// sdk/esp32/wake/test measures that difference on real speech.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "float_model.h"
#include "s3_engine.h"
#include "s3_kernels.h"

#if defined(ESP_PLATFORM)
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#endif

namespace {

using kws::detail::FloatMatrix;
using kws::detail::FloatModelFile;
using kws::detail::FloatOp;
using kws::detail::LayerKind;
using kws::detail::Storage;

int activation_bits = 16;
int batch_frames = 4;
constexpr int kMaxBatch = 8;
uint32_t kernel_cycles = 0, total_cycles = 0, profiled_frames = 0;

uint32_t cycles() {
#if defined(ESP_PLATFORM)
  return esp_cpu_get_cycle_count();
#else
  return 0;
#endif
}
size_t internal_bytes = 0, external_bytes = 0;
// Internal SRAM left for the rest of the firmware after the weights.
constexpr size_t kInternalReserve = 48 * 1024;

int round16(int n) { return (n + 15) & ~15; }

int8_t *alloc_aligned(size_t bytes) {
  bytes = (bytes + 15) & ~size_t(15);
#if defined(ESP_PLATFORM)
  void *p = nullptr;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > bytes + kInternalReserve &&
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > bytes + kInternalReserve)
    p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (p) {
    internal_bytes += bytes;
  } else {
    p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p)
      external_bytes += bytes;
  }
  return static_cast<int8_t *>(p);
#else
  internal_bytes += bytes;
  return static_cast<int8_t *>(std::aligned_alloc(16, bytes));
#endif
}

void free_aligned(void *p) {
#if defined(ESP_PLATFORM)
  heap_caps_free(p);
#else
  std::free(p);
#endif
}

// Working memory of the hot loops: internal SRAM first (PSRAM shares the data
// cache with the streamed weights), PSRAM only if internal memory is gone.
template <class T> struct Buffer {
  T *p = nullptr;
  Buffer() = default;
  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
  ~Buffer() { free_aligned(p); }
  bool alloc(size_t n) {
    const size_t bytes = (n * sizeof(T) + 15) & ~size_t(15);
#if defined(ESP_PLATFORM)
    p = static_cast<T *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!p)
      p = static_cast<T *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    p = static_cast<T *>(std::aligned_alloc(16, bytes));
#endif
    if (p)
      std::memset(p, 0, bytes);
    return p != nullptr;
  }
  T *data() const { return p; }
};

// One int8 matrix, rows zero-padded to `stride` (a multiple of 16).
struct Matrix {
  int rows = 0, cols = 0, stride = 0;
  int8_t *w = nullptr;
  std::vector<float> scale;     // per-row weight scale
  std::vector<int32_t> row_sum; // sum of the row's weights (16-bit split correction)
  Matrix() = default;
  Matrix(const Matrix &) = delete;
  Matrix &operator=(const Matrix &) = delete;
  ~Matrix() { free_aligned(w); }
};

bool supported(const FloatOp &op) {
  return !op.lowrank && op.w.storage == Storage::i8 && op.w.rows > 0;
}

bool pack(const FloatOp &op, Matrix &m) {
  const FloatMatrix &src = op.w;
  m.rows = src.rows;
  m.cols = src.cols;
  m.stride = round16(src.cols);
  m.w = alloc_aligned((size_t)m.rows * m.stride);
  if (!m.w)
    return false;
  std::memset(m.w, 0, (size_t)m.rows * m.stride);
  m.row_sum.assign(m.rows, 0);
  for (int r = 0; r < m.rows; ++r) {
    std::memcpy(m.w + (size_t)r * m.stride, src.i8.data() + (size_t)r * m.cols, m.cols);
    for (int c = 0; c < m.cols; ++c)
      m.row_sum[r] += src.i8[(size_t)r * m.cols + c];
  }
  m.scale = src.scale;
  return true;
}

// The S3 FPU has no divide or rounding instruction: libgcc's __divsf3 and
// libm's nearbyintf/fminf/fmaxf would be calls on every activation. These
// helpers use only multiply-adds and float<->int conversions.

// Float bits as an integer. A union compiles to register moves; a 4-byte
// memcpy stays a library call on Xtensa.
union Bits {
  float f;
  int32_t i;
};

// Round to nearest (ties away from zero) through a plain float-to-int conversion.
inline int32_t round_int(float v) { return (int32_t)(v + (v >= 0 ? 0.5f : -0.5f)); }

// exp(x): x = n ln2 + r with |r| <= ln2/2, e^r by a degree-6 Taylor polynomial
// (relative error under 2e-7 on that range), 2^n through the exponent bits.
inline float fast_exp(float x) {
  x = x < -87.0f ? -87.0f : x > 88.0f ? 88.0f : x;
  const float n = (float)round_int(x * 1.44269504f);
  const float r = (x - n * 0.693145752f) - n * 1.42860677e-6f;
  float p = 1.0f / 720.0f;
  p = p * r + 1.0f / 120.0f;
  p = p * r + 1.0f / 24.0f;
  p = p * r + 1.0f / 6.0f;
  p = p * r + 0.5f;
  p = p * r + 1.0f;
  p = p * r + 1.0f;
  Bits b{p};
  b.i += (int32_t)n << 23;
  return b.f;
}

// 1/d for 1 <= d <= 1e14: bit-trick estimate (12% error) and three Newton steps
// (error under 1e-7).
inline float reciprocal(float d) {
  Bits b{d};
  b.i = 0x7EF311C7 - b.i;
  float r = b.f;
  r = r * (2.0f - d * r);
  r = r * (2.0f - d * r);
  r = r * (2.0f - d * r);
  return r;
}

// Sigmoid by table: values and slopes at 1024 points on [-16, 16) (step
// 1/32), linearly interpolated (error under 2e-5; beyond the range the
// sigmoid is 0 or 1 within 1.2e-7). About 20 cycles instead of ~170 for exp and
// a reciprocal. A namespace-scope array in internal RAM, filled once by
// fill_sigmoid_table() before the first model is built.
struct SigmoidEntry {
  float v, d; // sigmoid(x_i) and sigmoid(x_i + 1/32) - sigmoid(x_i)
};
SigmoidEntry sigmoid_table[1024];
bool sigmoid_table_ready = false;

void fill_sigmoid_table() {
  if (sigmoid_table_ready)
    return;
  auto exact = [](int i) { return 1.0 / (1.0 + std::exp(-(i - 512) / 32.0)); };
  for (int i = 0; i < 1024; ++i) {
    sigmoid_table[i].v = (float)exact(i);
    sigmoid_table[i].d = (float)(exact(i + 1) - exact(i));
  }
  sigmoid_table_ready = true;
}

inline float sigmoid(float v) {
  float t = v * 32.0f + 512.0f;
  t = t < 0.0f ? 0.0f : t;
  t = t < 1023.999f ? t : 1023.999f; // also maps NaN to the top
  const int i = (int)t;
  const float f = t - (float)i;
  return sigmoid_table[i].v + f * sigmoid_table[i].d;
}

inline float fast_tanh(float v) { return 2.0f * sigmoid(2.0f * v) - 1.0f; }

void rmsnorm(const float *x, const float *gain, float *y, int n) {
  float ss = 0;
  for (int i = 0; i < n; ++i)
    ss += x[i] * x[i];
  const float rs = 1.0f / std::sqrt(ss / n + 1e-5f);
  for (int i = 0; i < n; ++i)
    y[i] = x[i] * rs * gain[i];
}

// Frames are computed in batches of `batch_frames`: every weight row is read
// from memory once and applied to the whole batch while it is in the cache,
// which divides the PSRAM traffic per frame by the batch size. The recurrence
// stays sequential (each frame's gates update the state in order); only the
// matrix products are shared. Posteriors therefore leave with a fixed delay of
// batch - 1 frames: step() returns frame t - (batch - 1), and the first
// batch - 1 calls return a pure-blank posterior.
class S3Acoustic final : public kws::detail::Acoustic {
public:
  S3Acoustic(const FloatModelFile &f, int batch) : file_(f), H_(f.hidden), B_(batch) {}

  bool init() {
    if (!pack(file_.embed, embed_) || !pack(file_.head, head_))
      return false;
    layers_ = std::make_unique<Layer[]>(file_.n_layers);
    widest_ = std::max(embed_.stride, head_.stride);
    for (int l = 0; l < file_.n_layers; ++l) {
      const auto &src = file_.layers[l];
      auto &dst = layers_[l];
      if (!pack(src.x, dst.x) || !pack(src.up, dst.up) || !pack(src.down, dst.down))
        return false;
      widest_ = std::max({widest_, dst.x.stride, dst.up.stride, dst.down.stride});
    }
    rows_ = std::max(head_.rows, 2 * H_);
    for (const auto &l : file_.layers)
      rows_ = std::max(rows_, l.up.rows);
    // Quantised inputs: hi and lo halves for every frame of the batch.
    quant_ = alloc_aligned((size_t)2 * B_ * widest_);
    if (!quant_)
      return false;
    for (int k = 0; k < 2 * B_; ++k) {
      in_[k] = quant_ + (size_t)k * widest_;
      out_[k] = nullptr;
    }
    if (!acc_.alloc((size_t)2 * B_ * rows_) || !a_.alloc((size_t)B_ * rows_) ||
        !m_.alloc((size_t)B_ * rows_))
      return false;
    for (int k = 0; k < 2 * B_; ++k)
      out_[k] = acc_.data() + (size_t)k * rows_;
    mel_.assign((size_t)B_ * 80, 0);
    x_.assign((size_t)B_ * 80, 0);
    r_.assign((size_t)B_ * H_, 0);
    n_.assign((size_t)B_ * H_, 0);
    s_.assign((size_t)file_.n_layers * H_, 0);
    logits_.assign((size_t)B_ * file_.n_phones, 0);
    ready_.assign((size_t)2 * B_ * file_.n_phones, 0);
    return true;
  }

  ~S3Acoustic() override { free_aligned(quant_); }

  void reset() override {
    // Frames queued before the reset belong to the old stream state.
    if (pending_)
      compute(pending_);
    std::fill(s_.begin(), s_.end(), 0.0f);
  }

  void step(const int16_t *mel80, int32_t *post) override {
    const uint32_t start = cycles();
    std::memcpy(mel_.data() + (size_t)pending_ * 80, mel80, 80 * sizeof(int16_t));
    if (++pending_ == B_)
      compute(B_);
    if (queued_ == 0) {
      // Still filling the first batch: report blank.
      std::fill(post, post + file_.n_phones, 0);
      post[0] = INT32_MAX;
    } else {
      const int n = file_.n_phones;
      std::memcpy(post, ready_.data() + (size_t)head_slot_ * n, n * sizeof(int32_t));
      head_slot_ = (head_slot_ + 1) % (2 * B_);
      --queued_;
    }
    total_cycles += cycles() - start;
    ++profiled_frames;
  }

  int delay() const { return B_ - 1; }

private:
  struct Layer {
    Matrix x, up, down;
  };

  static void add(float *y, const float *b, int n) {
    for (int i = 0; i < n; ++i)
      y[i] += b[i];
  }

  // Runs the first `count` queued frames through the network and appends their
  // posteriors to the output ring.
  void compute(int count) {
    const int H = H_, R = rows_;
    for (int b = 0; b < count; ++b) {
      const int16_t *mel = mel_.data() + (size_t)b * 80;
      float *x = x_.data() + (size_t)b * 80;
      for (int i = 0; i < 80; ++i) {
        const int k = i < 40 ? i : i - 40;
        const float v = (float)mel[i] * (1.0f / 1024.0f);
        x[i] = (v - file_.affine_offset[k]) * file_.affine_scale[k];
      }
    }
    apply(embed_, x_.data(), 80, r_.data(), H, count);
    for (int b = 0; b < count; ++b)
      add(r_.data() + (size_t)b * H, file_.embed_b.data(), H);
    const int candidate = file_.candidate_g ? 1 : file_.candidate_tanh ? 2 : 0;
    for (int l = 0; l < file_.n_layers; ++l) {
      const auto &src = file_.layers[l];
      const auto &w = layers_[l];
      float *s = s_.data() + (size_t)l * H;
      for (int b = 0; b < count; ++b)
        rmsnorm(r_.data() + (size_t)b * H, src.norm.data(), n_.data() + (size_t)b * H, H);
      apply(w.x, n_.data(), H, a_.data(), R, count);
      for (int b = 0; b < count; ++b) { // the recurrence: frames in order
        float *a = a_.data() + (size_t)b * R, *r = r_.data() + (size_t)b * H;
        add(a, src.b_ih.data(), 2 * H);
        for (int i = 0; i < H; ++i) {
          float ht = a[H + i];
          if (candidate == 1)
            ht = ht >= 0 ? ht + 0.5f : sigmoid(ht);
          else if (candidate == 2)
            ht = fast_tanh(ht);
          const float gate = sigmoid(a[i]); // sigmoid(-z) = 1 - sigmoid(z)
          s[i] = (1.0f - gate) * s[i] + gate * ht;
          r[i] += s[i];
        }
        rmsnorm(r, src.mlp_norm.data(), n_.data() + (size_t)b * H, H);
      }
      apply(w.up, n_.data(), H, m_.data(), R, count);
      for (int b = 0; b < count; ++b) {
        float *m = m_.data() + (size_t)b * R;
        add(m, src.up_b.data(), w.up.rows);
        for (int i = 0; i < w.up.rows; ++i)
          m[i] = m[i] * sigmoid(m[i]);
      }
      apply(w.down, m_.data(), R, a_.data(), R, count);
      for (int b = 0; b < count; ++b) {
        float *a = a_.data() + (size_t)b * R, *r = r_.data() + (size_t)b * H;
        add(a, src.down_b.data(), H);
        for (int i = 0; i < H; ++i)
          r[i] += a[i];
      }
    }
    for (int b = 0; b < count; ++b)
      rmsnorm(r_.data() + (size_t)b * H, file_.final_norm.data(), n_.data() + (size_t)b * H, H);
    apply(head_, n_.data(), H, logits_.data(), file_.n_phones, count);
    const int n = file_.n_phones;
    for (int b = 0; b < count; ++b) {
      const int slot = (head_slot_ + queued_) % (2 * B_);
      softmax_q31(logits_.data() + (size_t)b * n, ready_.data() + (size_t)slot * n);
      ++queued_;
    }
    pending_ = 0;
  }

  // y_b = W v_b for the first `count` vectors (v_b at v + b * vstride, y_b at
  // y + b * ystride), each quantised on its own scale (see the file comment).
  void apply(const Matrix &mat, const float *v, int vstride, float *y, int ystride, int count) {
    float unit[kMaxBatch];
    int used = 0; // inputs handed to the kernel (zero vectors are skipped)
    int which[kMaxBatch];
    for (int b = 0; b < count; ++b) {
      const float *vb = v + (size_t)b * vstride;
      float peak = 0;
      for (int i = 0; i < mat.cols; ++i)
        peak = std::max(peak, std::fabs(vb[i]));
      if (peak == 0) {
        std::fill(y + (size_t)b * ystride, y + (size_t)b * ystride + mat.rows, 0.0f);
        continue;
      }
      int8_t *hi = in_[2 * used], *lo = in_[2 * used + 1];
      std::memset(hi + mat.cols, 0, mat.stride - mat.cols);
      if (activation_bits == 8) {
        const float inv = 127.0f / peak;
        unit[used] = peak / 127.0f;
        for (int i = 0; i < mat.cols; ++i)
          hi[i] = (int8_t)round_int(vb[i] * inv);
        kernel_in_[used] = hi;
        kernel_out_[used] = out_[2 * used];
      } else {
        std::memset(lo + mat.cols, 0, mat.stride - mat.cols);
        const float inv = 32767.0f / peak;
        unit[used] = peak / 32767.0f;
        for (int i = 0; i < mat.cols; ++i) {
          const int32_t q = round_int(vb[i] * inv); // |q| <= 32767
          hi[i] = (int8_t)(q >> 8);                 // floor(q / 256)
          lo[i] = (int8_t)((q & 255) - 128);
        }
      }
      which[used++] = b;
    }
    if (!used)
      return;
    const uint32_t start = cycles();
    if (activation_bits == 8) {
      s3_matvec_multi_s8(mat.w, mat.rows, mat.stride, kernel_in_, used, kernel_out_);
    } else {
      s3_matvec_multi_s8(mat.w, mat.rows, mat.stride, (const int8_t *const *)in_, 2 * used, out_);
    }
    kernel_cycles += cycles() - start;
    for (int k = 0; k < used; ++k) {
      float *yb = y + (size_t)which[k] * ystride;
      if (activation_bits == 8) {
        const int32_t *acc = out_[2 * k];
        for (int r = 0; r < mat.rows; ++r)
          yb[r] = (float)acc[r] * (unit[k] * mat.scale[r]);
        continue;
      }
      const int32_t *hi = out_[2 * k], *lo = out_[2 * k + 1];
      for (int r = 0; r < mat.rows; ++r) {
        // |256 hi| <= 256 * 128 * 128 * 352 < 2^31 - 2^24: the int32 sum is exact.
        const int32_t dot = 256 * hi[r] + lo[r] + 128 * mat.row_sum[r];
        yb[r] = (float)dot * (unit[k] * mat.scale[r]);
      }
    }
  }

  // Same mapping as the engine's FloatModel: logits + bias -> Q31 posteriors.
  void softmax_q31(float *logits, int32_t *post) {
    const int n = file_.n_phones;
    float mx = -INFINITY;
    for (int i = 0; i < n; ++i) {
      logits[i] += file_.head_bias[i];
      mx = std::max(mx, logits[i]);
    }
    float sum = 0;
    for (int i = 0; i < n; ++i) {
      logits[i] = fast_exp(logits[i] - mx);
      sum += logits[i];
    }
    const float scale = 2147483648.0f * reciprocal(sum);
    for (int i = 0; i < n; ++i) {
      const float q = logits[i] * scale;
      post[i] = q >= 2147483520.0f ? INT32_MAX : (int32_t)(q + 0.5f);
    }
  }

  const FloatModelFile &file_;
  const int H_, B_;
  int widest_ = 0, rows_ = 0;
  Matrix embed_, head_;
  std::unique_ptr<Layer[]> layers_;
  int8_t *quant_ = nullptr;
  int8_t *in_[2 * kMaxBatch] = {};
  int32_t *out_[2 * kMaxBatch] = {};
  const int8_t *kernel_in_[kMaxBatch] = {};
  int32_t *kernel_out_[kMaxBatch] = {};
  Buffer<int32_t> acc_;
  Buffer<float> a_, m_;
  std::vector<int32_t> ready_;
  std::vector<int16_t> mel_;
  std::vector<float> x_, r_, n_, s_, logits_;
  int pending_ = 0;   // frames queued, not yet computed
  int queued_ = 0;    // computed posteriors not yet returned
  int head_slot_ = 0; // next posterior to return in the ring of 2 * batch
};

std::unique_ptr<kws::detail::Acoustic> make_s3(const FloatModelFile &f) {
  // The published minGRU stack with int8 dense maps; anything else stays on FloatModel.
  if (!f.published || f.out_proj || f.mixer != kws::detail::Mixer::none ||
      f.final_norm.size() != (size_t)f.hidden || !supported(f.embed) || !supported(f.head))
    return nullptr;
  for (const auto &l : f.layers)
    if (l.kind != LayerKind::mingru || !supported(l.x) || !supported(l.up) || !supported(l.down) ||
        l.inputs != f.hidden || l.norm.size() != (size_t)f.hidden ||
        l.mlp_norm.size() != (size_t)f.hidden)
      return nullptr;
  fill_sigmoid_table();
  auto model = std::make_unique<S3Acoustic>(f, batch_frames);
  if (!model->init())
    return nullptr;
  return model;
}

} // namespace

std::unique_ptr<kws::detail::Acoustic> esp_wake_s3_make(const FloatModelFile &file, int bits,
                                                        int batch) {
  activation_bits = bits == 8 ? 8 : 16;
  batch_frames = batch < 1 ? 1 : batch > kMaxBatch ? kMaxBatch : batch;
  return make_s3(file);
}

extern "C" void esp_wake_s3_enable(int bits, int batch) {
  activation_bits = bits == 8 ? 8 : 16;
  batch_frames = batch < 1 ? 1 : batch > kMaxBatch ? kMaxBatch : batch;
  kws::detail::set_float_acoustic_factory(make_s3);
}

extern "C" void esp_wake_s3_disable(void) { kws::detail::set_float_acoustic_factory(nullptr); }

extern "C" const char *esp_wake_s3_kernel(void) { return s3_kernel_name(); }

extern "C" void esp_wake_s3_memory(size_t *internal, size_t *external) {
  if (internal)
    *internal = internal_bytes;
  if (external)
    *external = external_bytes;
}

extern "C" void esp_wake_s3_profile(uint32_t *kernel, uint32_t *total, uint32_t *frames) {
  *kernel = kernel_cycles;
  *total = total_cycles;
  *frames = profiled_frames;
  kernel_cycles = total_cycles = profiled_frames = 0;
}

extern "C" int esp_wake_s3_selftest(void) {
  // Pseudo-random rows of every length the engine uses, SIMD vs plain C.
  alignas(16) static int8_t w[4 * 352], x[352];
  int32_t y[4];
  uint32_t seed = 12345;
  auto next = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return (int8_t)(seed >> 24);
  };
  int mismatches = 0;
  for (int cols : {16, 80, 176, 336, 352}) {
    for (int i = 0; i < 4 * cols; ++i)
      w[i] = next();
    for (int i = 0; i < cols; ++i)
      x[i] = next();
    w[0] = -128;
    x[0] = -128; // extreme products
    s3_matvec_s8(w, 4, cols, x, y);
    for (int r = 0; r < 4; ++r)
      mismatches += y[r] != s3_dot_s8_reference(w + r * cols, x, cols);
  }
  return mismatches;
}
