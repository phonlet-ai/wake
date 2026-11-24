#include "model.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "runtime_contract.h"

namespace kws::detail {

inline int clamp16(int32_t x) {
  return x < -32768 ? -32768 : x > 32767 ? 32767 : x;
}

inline int32_t Model::sigmoid_(int32_t v) {
  if (v < -8192)
    v = -8192;
  if (v > 8128)
    v = 8128;
  uint32_t u = (uint32_t)v & 0xffff;
  uint32_t idx = (u >> 6) & 255;
  uint32_t idx1 = (((u & 16383) + 63) >> 6) & 255;
  uint32_t frac = u & 63;
  return ((int32_t)kActLut[idx1] * (int32_t)frac +
          (int32_t)kActLut[idx] * (64 - (int32_t)frac)) >>
         6;
}

inline int32_t Model::tanh_(int32_t v) {
  int32_t x = clamp16(v * 2);
  int32_t t = sigmoid_(x);
  int32_t o = t * 2 - 32768;
  return o > -32768 ? o : -32768;
}

void Model::matmul_(int16_t *dst, const int16_t *x, const ModelMatrix &m,
                    bool accumulate) const {

  for (int c = 0; c < m.out_dim; ++c) {
    int32_t acc = (int32_t)m.bias[c] * 1024;
    const int16_t *wr = m.w16.data() + (size_t)c * m.in_dim;
    acc += dot_(x, wr, m.in_dim);
    int32_t out = (acc + 64) >> 7;
    if (accumulate)
      out += dst[c];
    dst[c] = (int16_t)clamp16(out);
  }
}

Model::Model(const ModelFile &file) : file_(file), dot_(selected_dot()) {
  v_.assign(80, 0);
  y_.assign((size_t)file_.n_groups * file_.hidden, 0);
  cstate_.assign((size_t)file_.n_groups * file_.hidden, 0);
  g_.assign((size_t)3 * file_.hidden, 0);
  e_.assign(file_.n_phones, 0);
#ifdef KWS_DEV
  taps_.resize(file_.n_groups);
  for (auto &t : taps_) {
    t.a.assign(file_.hidden, 0);
    t.bt.assign(file_.hidden, 0);
    t.c.assign(file_.hidden, 0);
    t.h.assign(file_.hidden, 0);
  }
#endif
  reset();
}

void Model::reset() {

  std::fill(v_.begin(), v_.end(), 0);
  std::fill(y_.begin(), y_.end(), 0);
  std::fill(cstate_.begin(), cstate_.end(), 0);
}

void Model::step(const int16_t *mel80, int32_t *post40, int16_t *affine80) {

  int16_t *v = v_.data();
  for (int i = 0; i < 80; ++i) {
    int k = i < 40 ? i : i - 40;
    int32_t t =
        ((int32_t)mel80[i] - file_.affine_bias[k]) * file_.affine_scale[k];
    v[i] = (int16_t)clamp16(t >> 10);
  }
  if (affine80)
    memcpy(affine80, v, 80 * sizeof(int16_t));

  for (int l = 0; l < file_.n_groups; ++l) {
    const int16_t *x = l == 0 ? v : y_.data() + (size_t)(l - 1) * file_.hidden;
    matmul_(g_.data(), x, file_.wx[l], false);
    matmul_(g_.data(), y_.data() + (size_t)l * file_.hidden, file_.wh[l], true);

    int16_t *h = cstate_.data() + (size_t)l * file_.hidden;
    int16_t *y = y_.data() + (size_t)l * file_.hidden;
    int H = file_.hidden;
#ifdef KWS_DEV
    int16_t *ta = taps_[l].a.data();
    int16_t *tbt = taps_[l].bt.data();
    int16_t *tc = taps_[l].c.data();
    int16_t *th = taps_[l].h.data();
#endif
    for (int i = 0; i < H; ++i) {
      int32_t A = sigmoid_(g_[i]);
      int32_t Bt = tanh_(g_[H + i]);
      int32_t C = sigmoid_(g_[2 * H + i]);
      int32_t hn = clamp16(((Bt * A + 524288) >> 20) +
                           (((32767 - A) * (int32_t)h[i] + 16384) >> 15));
      h[i] = (int16_t)hn;
      y[i] = (int16_t)clamp16((tanh_(hn) * C + 524288) >> 20);
#ifdef KWS_DEV
      ta[i] = (int16_t)A;
      tbt[i] = (int16_t)Bt;
      tc[i] = (int16_t)C;
      th[i] = (int16_t)hn;
#endif
    }
  }

  int16_t *logits = v;
  matmul_(logits, y_.data() + (size_t)(file_.n_groups - 1) * file_.hidden,
          file_.wout, false);
  int mx = logits[0];
  for (int i = 1; i < file_.n_phones; ++i)
    if (logits[i] > mx)
      mx = logits[i];

  int64_t sum = 0;
  for (int i = 0; i < file_.n_phones; ++i) {
    int32_t d = (int32_t)logits[i] - mx;
    int64_t e = 242;
    if (d >= -32767) {
      int32_t dc = d < -32768 ? -32768 : d;
      uint32_t u = (uint32_t)(-dc);
      if (u > 16383)
        u = 16383;
      int64_t p = (int64_t)kExpCoarse[u >> 7] * kExpFine[u & 127] + 1073741824;
      e = p >> 31;
      if (e > 2147483647)
        e = 2147483647;
    }
    e_[i] = (int32_t)e;
    sum += e;
  }
  int64_t inv = 0;
  if (sum > 0) {
    inv = (int64_t)4611686018427387904ULL / sum;
    if (inv > 2147483647)
      inv = 2147483647;
  }
  for (int i = 0; i < file_.n_phones; ++i) {
    int64_t o = ((int64_t)e_[i] * inv + 1073741824) >> 31;
    post40[i] = (int32_t)(o > 2147483647 ? 2147483647 : o);
  }
}

} // namespace kws::detail
