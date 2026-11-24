#include "model.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace kws::detail {
void Reader::need(size_t k) const {
  if (off > n || k > n - off)
    throw std::runtime_error("model file truncated");
}
uint32_t Reader::u32() {
  need(4);
  uint32_t v;
  memcpy(&v, b + off, 4);
  off += 4;
  return v;
}
uint16_t Reader::u16() {
  need(2);
  uint16_t v;
  memcpy(&v, b + off, 2);
  off += 2;
  return v;
}
uint8_t Reader::u8() {
  need(1);
  return b[off++];
}
void Reader::i16n(int16_t *dst, size_t k) {
  need(2 * k);
  memcpy(dst, b + off, 2 * k);
  off += 2 * k;
}

namespace {

void widen(ModelMatrix *m) { m->w16.assign(m->w.begin(), m->w.end()); }
} // namespace

ModelMatrix read_matrix(Reader &r) {
  ModelMatrix m;
  m.in_dim = (int)r.u32();
  m.out_dim = (int)r.u32();
  if (m.in_dim < 1 || m.in_dim > 1536 || m.out_dim < 1 || m.out_dim > 1536)
    throw std::runtime_error("invalid model matrix dimensions");
  r.need((size_t)m.out_dim + (size_t)m.in_dim * m.out_dim);
  m.bias.assign((int8_t *)r.b + r.off, (int8_t *)r.b + r.off + m.out_dim);
  r.off += m.out_dim;
  m.w.assign((int8_t *)r.b + r.off,
             (int8_t *)r.b + r.off + (size_t)m.in_dim * m.out_dim);
  r.off += (size_t)m.in_dim * m.out_dim;
  return m;
}

std::vector<uint8_t> read_file(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp)
    throw std::runtime_error("cannot open " + path);
  std::vector<uint8_t> buf;
  uint8_t chunk[1 << 16];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0)
    buf.insert(buf.end(), chunk, chunk + n);
  fclose(fp);
  return buf;
}

ModelFile load_model_body(Reader &r, int n_groups, const std::string &path) {
  ModelFile f;
  f.n_groups = n_groups;
  if (f.n_groups < 1 || f.n_groups > 64)
    throw std::runtime_error(path + ": implausible layer count");
  uint8_t has_affine = r.u8();
  if (has_affine) {
    uint32_t L = r.u32();
    if (L != 40)
      throw std::runtime_error(path + ": affine length != 40");
    f.affine_bias.resize(L);
    f.affine_scale.resize(L);
    r.i16n(f.affine_bias.data(), L);
    r.i16n(f.affine_scale.data(), L);
  }
  for (int i = 0; i < f.n_groups; ++i) {
    f.wx.push_back(read_matrix(r));
    f.wh.push_back(read_matrix(r));
  }
  f.wout = read_matrix(r);
  f.hidden = f.wh[0].in_dim;

  if (f.hidden < 1 || f.hidden > 512)
    throw std::runtime_error(path + ": implausible hidden size " +
                             std::to_string(f.hidden));
  for (const auto &m : f.wh)
    if (m.in_dim != f.hidden || m.out_dim != 3 * f.hidden)
      throw std::runtime_error(path + ": W_h dims inconsistent (need 3-gate)");
  for (const auto &m : f.wx)
    if (m.out_dim != 3 * f.hidden)
      throw std::runtime_error(path + ": W_x cols != 3*hidden");

  if (f.wx[0].in_dim != 80)
    throw std::runtime_error(
        path + ": layer-0 W_x takes " + std::to_string(f.wx[0].in_dim) +
        " inputs, but the model is fed 80 stacked mel values");
  for (size_t l = 1; l < f.wx.size(); ++l)
    if (f.wx[l].in_dim != f.hidden)
      throw std::runtime_error(path + ": W_x rows != hidden at layer " +
                               std::to_string(l));
  if (f.wout.in_dim != f.hidden)
    throw std::runtime_error(path + ": W_out rows != hidden");
  f.n_phones = f.wout.out_dim;

  if (f.n_phones < 1 || f.n_phones > 80)
    throw std::runtime_error(path + ": implausible class count " +
                             std::to_string(f.n_phones));
  for (auto *m : {&f.wout})
    widen(m);
  for (auto &m : f.wx)
    widen(&m);
  for (auto &m : f.wh)
    widen(&m);
  return f;
}

} // namespace kws::detail
