#pragma once
// Float acoustic container path: CIFG layers of any width whose affine
// maps are dense or low rank, with weights stored fp32, fp16 or int8 (one fp32
// scale per stored row). Compute is fp32; weights are read in their stored
// format by the dot kernels, never expanded to a resident fp32 copy.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "acoustic.h"

namespace kws::detail {

enum class Storage : uint8_t { f32 = 0, f16 = 1, i8 = 2 };

// One stored row-major matrix [rows, cols].
struct FloatMatrix {
  int rows = 0, cols = 0;
  Storage storage = Storage::f32;
  std::vector<float> f32;
  std::vector<uint16_t> f16;
  std::vector<int8_t> i8;
  std::vector<float> scale; // i8 only: one per row
  size_t values() const { return size_t(rows) * cols; }
  size_t bytes() const;
};

// y = M v, M dense [rows, cols] or A [rows, rank] @ B [rank, cols].
struct FloatOp {
  bool lowrank = false;
  int rows = 0, cols = 0, rank = 0;
  FloatMatrix w; // dense
  FloatMatrix a, b; // low rank
  size_t parameters() const {
    return lowrank ? size_t(rank) * (rows + cols) : size_t(rows) * cols;
  }
};

// joint / split: CIFG. The others are input-gated recurrences with one state
// vector s per layer, s_t = a_t * s_{t-1} + b_t, gates from the layer input only
// (experiments/parallel_rnn_20260924/layers.py):
//   mingru  [z; h~] = W x + b            s = sigmoid(-z) s + sigmoid(z) h~,  out s
//   igcifg  [i; g; o] = W x + b          s = sigmoid(-i) s + sigmoid(i) tanh(g),
//                                        out sigmoid(o) tanh(s)
//   minlstm [f; i; h~] = W x + b         f' = f/(f+i), i' = i/(f+i) (f, i sigmoids)
//                                        s = f' s + i' h~,  out s
// then the model's mixer (none | tanh | silu) on every layer's output.
// residual (input-gated only): layer 0 as above; every later layer l reads
// RMSNorm(r) (gain norm) and adds its output to the stream r; RMSNorm(r) with
// final_norm feeds the head. candidate_g (mingru): h~ -> h~ + 0.5 if h~ >= 0,
// else sigmoid(h~).
// published (Feng et al. 2024's stack, input-gated only): r = E x + e; per layer
// r += layer(RMSNorm(r, norm)); r += down(silu(up(RMSNorm(r, mlp_norm)))); the head
// reads RMSNorm(r, final_norm). Every layer takes H inputs. out_proj: the recurrent
// branch passes through a learned H x H map (out, out_b) before the residual add.
enum class LayerKind : uint8_t { joint = 1, split = 2, mingru = 3, igcifg = 4, minlstm = 5 };
enum class Mixer : uint8_t { none = 0, tanh = 1, silu = 2 };

inline bool input_gated(LayerKind k) { return k == LayerKind::mingru || k == LayerKind::igcifg || k == LayerKind::minlstm; }
// rows of the layer's pre-activation, in units of H
inline int gate_rows(LayerKind k) { return k == LayerKind::mingru ? 2 : 3; }

struct FloatLayer {
  LayerKind kind = LayerKind::split;
  int inputs = 0;
  std::vector<float> b_ih, b_hh; // input-gated kinds: b_ih only, b_hh empty
  std::vector<float> norm;       // residual models, layers >= 1: RMSNorm gain [H]
  std::vector<float> mlp_norm, up_b, down_b; // published block
  FloatOp up, down;                          // [E, H], [H, E]
  FloatOp out;                               // out_proj: [H, H]
  std::vector<float> out_b;
  FloatOp u;    // joint: [3H, inputs + H]
  FloatOp x, h; // split: [3H, inputs], [3H, H]; input-gated: x [G*H, inputs]
};

struct FloatModelFile {
  int n_layers = 0, hidden = 0, n_phones = 0;
  Mixer mixer = Mixer::none;
  bool residual = false, candidate_g = false, published = false, candidate_tanh = false,
       out_proj = false;
  FloatOp embed;                 // published: [H, 80]
  std::vector<float> embed_b;
  std::vector<float> final_norm; // residual models: RMSNorm gain before the head
  std::vector<float> affine_offset, affine_scale; // 40 each, mel/1024 units
  std::vector<FloatLayer> layers;
  FloatOp head;
  std::vector<float> head_bias;
  size_t parameters() const;
  size_t tensor_bytes() const;
};

bool is_float_container(const std::vector<uint8_t> &bytes);
FloatModelFile load_float_model(const std::vector<uint8_t> &bytes,
                                const std::string &path);

// Row dot products against a stored matrix; x is fp32. Summation order is
// fixed (eight interleaved partial sums of fused multiply-adds, then a fixed
// reduction), so the portable (fmaf) and AVX2/FMA kernels give bit-identical
// results.
void matvec(const FloatMatrix &m, const float *x, float *y);
// The same dot products for K vectors: each stored row is read once and
// applied to every vector while it is in cache. y[k] is bit-identical to
// matvec(m, x[k], y[k]).
void matvec_batch(const FloatMatrix &m, const float *const *x, float *const *y, int k);
const char *float_kernel();

// K independent streams of one float model stepped in lockstep (shared weight
// reads). Stream s's results are bit-identical to a FloatModel run alone.
class FloatBatch {
public:
  FloatBatch(const FloatModelFile &file, int streams);
  void reset(int s);
  // mel[s] / post[s] for the streams with active[s] != 0
  void step(const int16_t *const *mel, int32_t *const *post, const uint8_t *active);

private:
  void apply(const FloatOp &op, int n, const float *const *v, float *const *y);
  const FloatModelFile &file_;
  int K_;
  std::vector<std::vector<float>> x_, h_, c_, a_, z_, v_, tmp_, logits_, r_, m_;
  std::vector<double> e_;
  std::vector<const float *> in_;
  std::vector<float *> out_, out2_;
  std::vector<int> idx_;
};

// Optional platform backend for float containers, for example the ESP32-S3
// SIMD engine in sdk/esp32/wake. Sessions created after it is set ask the
// factory first and fall back to FloatModel when it returns null (a model it
// does not support). Unset (the default) keeps FloatModel everywhere.
using FloatAcousticFactory = std::unique_ptr<Acoustic> (*)(const FloatModelFile &);
void set_float_acoustic_factory(FloatAcousticFactory factory);

class FloatModel final : public Acoustic {
public:
  explicit FloatModel(const FloatModelFile &file);
  void reset() override;
  void step(const int16_t *mel80, int32_t *post) override;
  // Last frame's logits (diagnostics).
  const std::vector<float> &logits() const { return logits_; }

private:
  void apply(const FloatOp &op, const float *v, float *y);
  const FloatModelFile &file_;
  std::vector<float> x_, h_, c_, a_, z_, v_, tmp_, logits_, r_, m_;
  std::vector<double> e_;
};

} // namespace kws::detail
