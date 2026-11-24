// Float acoustic container (kind 1). Little-endian throughout.
//
//   "phonletwake" u32 kind(=1) u32 version(=1) u32 header_len u32 frontend_id
//   u16 n_layers u16 hidden u16 n_classes u16 n_mels(40) u16 stack(2)
//   u16 flags (bits 0-1: mixer after every layer, 0 none | 1 tanh | 2 silu;
//              bit 2: residual; bit 3: mingru candidate g; bit 4: published;
//              bit 5: mingru candidate tanh; bit 6: published out_proj)
//   -- body at header_len --
//   f32 affine_offset[40] f32 affine_scale[40]         (mel/1024 units)
//   per layer:
//     u8 kind (1 joint | 2 split | 3 mingru | 4 igcifg | 5 minlstm) u32 inputs
//     CIFG kinds: f32 b_ih[3H] f32 b_hh[3H]
//       joint: op u [3H, inputs+H];  split: op x [3H, inputs], op h [3H, H]
//     input-gated kinds (G = 2 mingru, 3 otherwise): f32 b[G*H], op x [G*H, inputs];
//       residual models, layers >= 1: f32 norm[H] first
//   residual models: f32 final_norm[H]
//   published models (all layers take H inputs): after the affine, op embed [H, 80]
//     and f32 embed_b[H]; per layer f32 norm[H], f32 b[G*H], op x [G*H, H],
//     [out_proj: op out [H, H], f32 out_b[H]]
//     f32 mlp_norm[H], op up [E, H], f32 up_b[E], op down [H, E], f32 down_b[H];
//     then f32 final_norm[H]
//   op head [classes, H]; f32 head_bias[classes]
//   "phonletwake" (trailer)
// op: u8 lowrank u8 storage(0 f32 | 1 f16 | 2 i8) u32 rows u32 cols u32 rank
//     dense: matrix [rows, cols]; low rank: A [rows, rank], B [rank, cols]
//     (all matrices of an op share its storage)
// matrix: f32: values; f16: u16 values; i8: f32 scale[rows], then i8 values
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include "container.h"
#include "float_model.h"
#include "model.h"
#include "runtime_contract.h"

namespace kws::detail {

namespace {

constexpr const char *kKindName = "float container";

float f32(Reader &r) {
  uint32_t v = r.u32();
  float f;
  memcpy(&f, &v, 4);
  return f;
}

std::vector<float> f32n(Reader &r, size_t n) {
  r.need(4 * n);
  std::vector<float> out(n);
  memcpy(out.data(), r.b + r.off, 4 * n);
  r.off += 4 * n;
  for (float v : out)
    if (!std::isfinite(v))
      throw std::runtime_error("float container: non-finite value");
  return out;
}

FloatMatrix read_matrix(Reader &r, Storage storage, int rows, int cols) {
  FloatMatrix m;
  m.rows = rows;
  m.cols = cols;
  m.storage = storage;
  size_t n = (size_t)rows * cols;
  switch (storage) {
  case Storage::f32:
    m.f32 = f32n(r, n);
    break;
  case Storage::f16:
    r.need(2 * n);
    m.f16.resize(n);
    memcpy(m.f16.data(), r.b + r.off, 2 * n);
    r.off += 2 * n;
    for (uint16_t h : m.f16)
      if ((h & 0x7c00) == 0x7c00)
        throw std::runtime_error("float container: non-finite fp16 value");
    break;
  case Storage::i8:
    m.scale = f32n(r, rows);
    for (float s : m.scale)
      if (!(s > 0))
        throw std::runtime_error("float container: int8 row scale must be positive");
    r.need(n);
    m.i8.assign((const int8_t *)r.b + r.off, (const int8_t *)r.b + r.off + n);
    r.off += n;
    break;
  default:
    throw std::runtime_error("float container: unknown storage");
  }
  return m;
}

FloatOp read_op(Reader &r, int rows, int cols, const char *what) {
  FloatOp op;
  op.lowrank = r.u8() != 0;
  uint8_t storage = r.u8();
  if (storage > 2)
    throw std::runtime_error(std::string("float container: unknown storage in ") + what);
  op.rows = (int)r.u32();
  op.cols = (int)r.u32();
  op.rank = (int)r.u32();
  if (rows < 0) // any row count (the caller reads it from the op)
    rows = op.rows;
  if (rows < 1 || rows > 65536 || op.rows != rows || op.cols != cols)
    throw std::runtime_error(std::string("float container: shape mismatch in ") + what + ": " +
                             std::to_string(op.rows) + "x" + std::to_string(op.cols) +
                             ", expected " + std::to_string(rows) + "x" + std::to_string(cols));
  auto s = (Storage)storage;
  if (op.lowrank) {
    if (op.rank < 1 || op.rank > std::min(rows, cols))
      throw std::runtime_error(std::string("float container: invalid rank in ") + what);
    op.a = read_matrix(r, s, rows, op.rank);
    op.b = read_matrix(r, s, op.rank, cols);
  } else {
    if (op.rank != 0)
      throw std::runtime_error(std::string("float container: dense op with a rank in ") + what);
    op.w = read_matrix(r, s, rows, cols);
  }
  return op;
}

} // namespace

bool is_float_container(const std::vector<uint8_t> &bytes) {
  return container_kind(bytes) == kContainerKindFloat;
}

FloatModelFile load_float_model(const std::vector<uint8_t> &buf, const std::string &path) {
  if (!is_float_container(buf))
    throw std::runtime_error(path + ": expected a phonletwake float container");
  Reader r{buf.data(), buf.size(), kContainerMagicSize};
  if (r.u32() != kContainerKindFloat)
    throw std::runtime_error(path + ": wrong container kind");
  uint32_t version = r.u32();
  if (version != kContainerVersion)
    throw std::runtime_error(path + ": container version " + std::to_string(version));
  uint32_t header_len = r.u32();
  if (header_len < kContainerHeaderReserve || header_len > buf.size())
    throw std::runtime_error(path + ": implausible header length");
  if (r.u32() != kFrontendId)
    throw std::runtime_error(path + ": trained against a different frontend");
  FloatModelFile f;
  f.n_layers = r.u16();
  f.hidden = r.u16();
  f.n_phones = r.u16();
  int mels = r.u16(), stack = r.u16();
  uint16_t flags = r.u16();
  if ((flags & 3) > 2 || flags > 127 || ((flags & 8) && (flags & 32)))
    throw std::runtime_error(path + ": unknown float container flags");
  f.mixer = (Mixer)(flags & 3);
  f.residual = flags & 4;
  f.candidate_g = flags & 8;
  f.published = flags & 16;
  f.candidate_tanh = flags & 32;
  f.out_proj = flags & 64;
  if (f.out_proj && !f.published)
    throw std::runtime_error(path + ": out_proj belongs to the published block");
  if (f.published && f.residual)
    throw std::runtime_error(path + ": published and residual are separate layouts");
  if (f.residual && f.mixer != Mixer::none)
    throw std::runtime_error(path + ": a residual model has no mixer");
  if (mels != 40 || stack != 2)
    throw std::runtime_error(path + ": the model must take 40 mels x 2 stacked");
  if (f.n_layers < 1 || f.n_layers > 16 || f.hidden < 1 || f.hidden > 2048 ||
      f.n_phones < 2 || f.n_phones > 4096)
    throw std::runtime_error(path + ": implausible float container dimensions");
  r.off = header_len;
  const int H = f.hidden;
  f.affine_offset = f32n(r, 40);
  f.affine_scale = f32n(r, 40);
  for (float s : f.affine_scale)
    if (!(s > 0))
      throw std::runtime_error(path + ": nonpositive affine scale");
  if (f.published) {
    f.embed = read_op(r, H, 80, "embedding");
    f.embed_b = f32n(r, H);
  }
  for (int l = 0; l < f.n_layers; ++l) {
    FloatLayer layer;
    uint8_t kind = r.u8();
    if (kind < 1 || kind > 5)
      throw std::runtime_error(path + ": unknown layer kind");
    layer.kind = (LayerKind)kind;
    layer.inputs = (int)r.u32();
    int want = l == 0 && !f.published ? 80 : H;
    if (layer.inputs != want)
      throw std::runtime_error(path + ": layer " + std::to_string(l) + " takes " +
                               std::to_string(layer.inputs) + " inputs, expected " +
                               std::to_string(want));
    if (input_gated(layer.kind)) {
      if (l && input_gated(f.layers[0].kind) != true)
        throw std::runtime_error(path + ": mixed CIFG and input-gated layers");
      int G = gate_rows(layer.kind);
      if ((f.residual && l) || f.published)
        layer.norm = f32n(r, H);
      layer.b_ih = f32n(r, (size_t)G * H);
      layer.x = read_op(r, G * H, layer.inputs, "input-gated layer");
      if (f.published) {
        if (f.out_proj) {
          layer.out = read_op(r, H, H, "recurrent output projection");
          layer.out_b = f32n(r, H);
        }
        layer.mlp_norm = f32n(r, H);
        layer.up = read_op(r, -1, H, "MLP up");
        layer.up_b = f32n(r, layer.up.rows);
        layer.down = read_op(r, H, layer.up.rows, "MLP down");
        layer.down_b = f32n(r, H);
      }
      f.layers.push_back(std::move(layer));
      continue;
    }
    if (l && input_gated(f.layers[0].kind))
      throw std::runtime_error(path + ": mixed CIFG and input-gated layers");
    layer.b_ih = f32n(r, 3 * H);
    layer.b_hh = f32n(r, 3 * H);
    if (layer.kind == LayerKind::joint) {
      layer.u = read_op(r, 3 * H, layer.inputs + H, "joint layer");
    } else {
      layer.x = read_op(r, 3 * H, layer.inputs, "input map");
      layer.h = read_op(r, 3 * H, H, "recurrent map");
    }
    f.layers.push_back(std::move(layer));
  }
  if ((f.mixer != Mixer::none || f.residual || f.candidate_g || f.candidate_tanh) &&
      !input_gated(f.layers[0].kind))
    throw std::runtime_error(path + ": mixer/residual/candidate need input-gated layers");
  if ((f.candidate_g || f.candidate_tanh) && f.layers[0].kind != LayerKind::mingru)
    throw std::runtime_error(path + ": candidate g is minGRU's");
  if (f.residual || f.published)
    f.final_norm = f32n(r, H);
  f.head = read_op(r, f.n_phones, H, "head");
  f.head_bias = f32n(r, f.n_phones);
  r.need(kContainerMagicSize);
  if (memcmp(r.b + r.off, kContainerMagic, kContainerMagicSize) != 0)
    throw std::runtime_error(path + ": container trailer missing");
  r.off += kContainerMagicSize;
  if (r.off != r.n)
    throw std::runtime_error(path + ": trailing bytes after the weight body");
  return f;
}

} // namespace kws::detail
