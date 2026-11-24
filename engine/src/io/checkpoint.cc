// Direct loading of cifg_lstm training checkpoints (torch.save zip files), so a
// run's u*.ckpt can be given to the engine as --model without an export step.
//
// Reads the zip central directory (stored entries only, as torch writes them),
// runs a small pickle machine over data.pkl that understands the opcodes and
// the five globals a torch state checkpoint uses, and resolves tensors to
// their raw little-endian storages under data/<key>. Only "model" (the state
// dict) and "config" are used; everything else in the pickle is parsed and
// ignored.
//
//   config.float_weights true  -> FloatModelFile, dense fp32, as trained
//   otherwise (boxed int8)     -> ModelFile (quantized container path), codes = rint(w*128)
//                                 with every weight required inside the box
//                                 [-1, 127/128]; affine = rint(x*1024). This is
//                                 cifg_lstm.export's arithmetic; a weight off
//                                 the grid is refused, never clipped.
// The affine must be frozen (checkpoints before the freeze have no deployed
// model). Auxiliary heads are ignored.
//
// parallel-rnn-checkpoint-v1 (experiments/parallel_rnn_20260924/train_parallel.py):
// spec.model mingru | igcifg | minlstm, spec.width, spec.mixer, optional spec.block
// (plain | residual) and spec.candidate (linear | g); tensors offset, scale (the
// frozen affine), layers.N.proj.{weight,bias}, head.{weight,bias}; residual:
// norms.{N-1}.weight before layer N >= 1, final_norm.weight; published: embed.*,
// norms.N / mlp_norms.N .weight, mlps.N.{up,down}.{weight,bias}, final_norm.weight
//   -> FloatModelFile with input-gated layers, dense fp32, as trained.
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "checkpoint.h"

namespace kws::detail {

namespace {

[[noreturn]] void fail(const std::string &what) {
  throw std::runtime_error("checkpoint: " + what);
}

uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) { return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16; }

// ------------------------------------------------------------------- zip
struct Zip {
  const std::vector<uint8_t> &b;
  std::map<std::string, std::pair<size_t, size_t>> entries; // name -> (offset, size)
  explicit Zip(const std::vector<uint8_t> &bytes) : b(bytes) {
    if (b.size() < 22)
      fail("not a zip file");
    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 22 + 1; i-- > 0 && b.size() - i <= 22 + 65535;)
      if (le32(&b[i]) == 0x06054b50) {
        eocd = i;
        break;
      }
    if (eocd == std::string::npos)
      fail("zip end record not found");
    size_t count = le16(&b[eocd + 10]);
    size_t dir = le32(&b[eocd + 16]);
    if (dir == 0xffffffff || count == 0xffff)
      fail("zip64 checkpoints are not supported");
    size_t p = dir;
    for (size_t i = 0; i < count; ++i) {
      if (p + 46 > b.size() || le32(&b[p]) != 0x02014b50)
        fail("bad zip central directory");
      uint16_t method = le16(&b[p + 10]);
      size_t size = le32(&b[p + 20]);
      size_t name_len = le16(&b[p + 28]), extra = le16(&b[p + 30]), comment = le16(&b[p + 32]);
      size_t local = le32(&b[p + 42]);
      std::string name((const char *)&b[p + 46], name_len);
      p += 46 + name_len + extra + comment;
      if (method != 0)
        fail("compressed zip entry " + name);
      if (local + 30 > b.size() || le32(&b[local]) != 0x04034b50)
        fail("bad zip local header");
      size_t data = local + 30 + le16(&b[local + 26]) + le16(&b[local + 28]);
      if (data + size > b.size())
        fail("zip entry beyond end of file");
      entries[name] = {data, size};
    }
  }
  std::pair<const uint8_t *, size_t> find_suffix(const std::string &suffix) const {
    for (auto &e : entries)
      if (e.first.size() >= suffix.size() &&
          e.first.compare(e.first.size() - suffix.size(), suffix.size(), suffix) == 0)
        return {&b[e.second.first], e.second.second};
    fail("zip entry not found: *" + suffix);
  }
};

// ---------------------------------------------------------------- pickle
struct Value;
using V = std::shared_ptr<Value>;
struct Value {
  enum Kind { None, Bool, Int, Float, Str, Bytes, List, Tuple, Dict, Global, Object, Storage, Tensor, Mark };
  Kind kind = None;
  bool b = false;
  int64_t i = 0;
  double f = 0;
  std::string s;                        // str/bytes, global "module name", storage dtype
  std::vector<V> items;                 // list/tuple, object args
  std::vector<std::pair<V, V>> dict;
  std::string key;                      // storage key
  V storage;                            // tensor
  int64_t offset = 0;
  std::vector<int64_t> size, stride;
};
V make(Value::Kind k) {
  auto v = std::make_shared<Value>();
  v->kind = k;
  return v;
}

int64_t as_int(const V &v) {
  if (v->kind == Value::Int)
    return v->i;
  if (v->kind == Value::Bool)
    return v->b;
  fail("expected an integer");
}
std::vector<int64_t> int_tuple(const V &v) {
  std::vector<int64_t> out;
  for (auto &x : v->items)
    out.push_back(as_int(x));
  return out;
}

V unpickle(const uint8_t *p, size_t n) {
  std::vector<V> stack;
  std::vector<size_t> marks;
  std::map<int64_t, V> memo;
  size_t off = 0;
  auto need = [&](size_t k) {
    if (off + k > n)
      fail("pickle truncated");
  };
  auto pop = [&]() {
    if (stack.empty())
      fail("pickle stack underflow");
    V v = stack.back();
    stack.pop_back();
    return v;
  };
  auto pop_mark = [&]() {
    if (marks.empty())
      fail("pickle mark underflow");
    size_t m = marks.back();
    marks.pop_back();
    std::vector<V> out(stack.begin() + m, stack.end());
    stack.resize(m);
    return out;
  };
  auto text = [&](size_t len) {
    need(len);
    std::string s((const char *)p + off, len);
    off += len;
    return s;
  };
  auto line = [&]() {
    size_t e = off;
    while (e < n && p[e] != '\n')
      ++e;
    if (e == n)
      fail("pickle line truncated");
    std::string s((const char *)p + off, e - off);
    off = e + 1;
    return s;
  };
  auto set_items = [&](V d, const std::vector<V> &kv) {
    if (d->kind != Value::Dict)
      fail("SETITEMS on a non-dict");
    if (kv.size() % 2)
      fail("odd SETITEMS");
    for (size_t k = 0; k < kv.size(); k += 2)
      d->dict.emplace_back(kv[k], kv[k + 1]);
  };
  auto call = [&](V callable, V args) -> V {
    if (callable->kind != Value::Global)
      fail("call of a non-global");
    if (callable->s == "collections OrderedDict")
      return make(Value::Dict);
    if (callable->s == "torch._utils _rebuild_tensor_v2") {
      auto &a = args->items;
      if (a.size() < 4 || a[0]->kind != Value::Storage)
        fail("unexpected _rebuild_tensor_v2 arguments");
      V t = make(Value::Tensor);
      t->storage = a[0];
      t->offset = as_int(a[1]);
      t->size = int_tuple(a[2]);
      t->stride = int_tuple(a[3]);
      return t;
    }
    V o = make(Value::Object); // anything else: opaque (rng state, numpy, ...)
    o->s = callable->s;
    o->items = args->items;
    return o;
  };
  while (true) {
    need(1);
    uint8_t op = p[off++];
    switch (op) {
    case 0x80: need(1); ++off; break;                                   // PROTO
    case 0x95: need(8); off += 8; break;                                // FRAME
    case '.': return pop();                                             // STOP
    case '(': marks.push_back(stack.size()); break;                     // MARK
    case '}': stack.push_back(make(Value::Dict)); break;                // EMPTY_DICT
    case ']': stack.push_back(make(Value::List)); break;                // EMPTY_LIST
    case ')': stack.push_back(make(Value::Tuple)); break;               // EMPTY_TUPLE
    case 0x8f: stack.push_back(make(Value::List)); break;               // EMPTY_SET
    case 't': { V t = make(Value::Tuple); t->items = pop_mark(); stack.push_back(t); break; }
    case 0x85: case 0x86: case 0x87: {                                  // TUPLE1..3
      size_t k = op - 0x84;
      if (stack.size() < k) fail("pickle stack underflow");
      V t = make(Value::Tuple);
      t->items.assign(stack.end() - k, stack.end());
      stack.resize(stack.size() - k);
      stack.push_back(t);
      break;
    }
    case 'q': need(1); memo[p[off++]] = stack.back(); break;            // BINPUT
    case 'r': need(4); memo[le32(p + off)] = stack.back(); off += 4; break;
    case 0x94: memo[(int64_t)memo.size()] = stack.back(); break;        // MEMOIZE
    case 'h': need(1); stack.push_back(memo.at(p[off++])); break;       // BINGET
    case 'j': need(4); stack.push_back(memo.at(le32(p + off))); off += 4; break;
    case 'J': { need(4); V v = make(Value::Int); v->i = (int32_t)le32(p + off); off += 4; stack.push_back(v); break; }
    case 'K': { need(1); V v = make(Value::Int); v->i = p[off++]; stack.push_back(v); break; }
    case 'M': { need(2); V v = make(Value::Int); v->i = le16(p + off); off += 2; stack.push_back(v); break; }
    case 0x8a: {                                                        // LONG1
      need(1);
      size_t len = p[off++];
      need(len);
      if (len > 8) fail("LONG1 wider than 64 bits");
      uint64_t u = 0;
      for (size_t k = 0; k < len; ++k)
        u |= uint64_t(p[off + k]) << (8 * k);
      if (len && len < 8 && (p[off + len - 1] & 0x80))
        u |= ~uint64_t(0) << (8 * len);
      off += len;
      V v = make(Value::Int);
      v->i = (int64_t)u;
      stack.push_back(v);
      break;
    }
    case 'G': {                                                         // BINFLOAT (big endian)
      need(8);
      uint64_t u = 0;
      for (int k = 0; k < 8; ++k)
        u = u << 8 | p[off + k];
      off += 8;
      V v = make(Value::Float);
      memcpy(&v->f, &u, 8);
      stack.push_back(v);
      break;
    }
    case 'X': { need(4); size_t l = le32(p + off); off += 4; V v = make(Value::Str); v->s = text(l); stack.push_back(v); break; }
    case 0x8c: { need(1); size_t l = p[off++]; V v = make(Value::Str); v->s = text(l); stack.push_back(v); break; }
    case 'B': { need(4); size_t l = le32(p + off); off += 4; V v = make(Value::Bytes); v->s = text(l); stack.push_back(v); break; }
    case 'C': { need(1); size_t l = p[off++]; V v = make(Value::Bytes); v->s = text(l); stack.push_back(v); break; }
    case 'N': stack.push_back(make(Value::None)); break;
    case 0x88: { V v = make(Value::Bool); v->b = true; stack.push_back(v); break; }
    case 0x89: { V v = make(Value::Bool); v->b = false; stack.push_back(v); break; }
    case 'c': { V g = make(Value::Global); std::string m = line(); g->s = m + " " + line(); stack.push_back(g); break; }
    case 0x93: { V name = pop(), mod = pop(); V g = make(Value::Global); g->s = mod->s + " " + name->s; stack.push_back(g); break; }
    case 'R': { V args = pop(), f = pop(); stack.push_back(call(f, args)); break; }   // REDUCE
    case 0x81: { V args = pop(), cls = pop(); stack.push_back(call(cls, args)); break; } // NEWOBJ
    case 'b': {                                                         // BUILD
      V state = pop();
      V obj = stack.back();
      if (obj->kind == Value::Dict && state->kind == Value::Dict)
        for (auto &kv : state->dict)
          obj->dict.push_back(kv);
      break;                                                            // other state ignored
    }
    case 'Q': {                                                         // BINPERSID
      V pid = pop();
      auto &a = pid->items;
      if (pid->kind != Value::Tuple || a.size() < 5 || a[0]->s != "storage")
        fail("unexpected persistent id");
      V st = make(Value::Storage);
      st->s = a[1]->s;          // "torch FloatStorage"
      st->key = a[2]->s;
      st->i = as_int(a[4]);     // numel
      stack.push_back(st);
      break;
    }
    case 's': { V v = pop(), k = pop(); set_items(stack.back(), {k, v}); break; }
    case 'u': { auto kv = pop_mark(); set_items(stack.back(), kv); break; }
    case 'a': { V v = pop(); stack.back()->items.push_back(v); break; }
    case 'e': { auto items = pop_mark(); auto &l = stack.back()->items; l.insert(l.end(), items.begin(), items.end()); break; }
    case 0x90: { auto items = pop_mark(); auto &l = stack.back()->items; l.insert(l.end(), items.begin(), items.end()); break; }
    case 0x91: { V t = make(Value::List); t->items = pop_mark(); stack.push_back(t); break; } // FROZENSET
    case '0': pop(); break;
    case '1': pop_mark(); break;
    case '2': stack.push_back(stack.back()); break;
    default: {
      char buf[64];
      snprintf(buf, sizeof buf, "unsupported pickle opcode 0x%02x at %zu", op, off - 1);
      fail(buf);
    }
    }
  }
}

V get(const V &dict, const std::string &key) {
  if (!dict || dict->kind != Value::Dict)
    fail("expected a dict looking up " + key);
  for (auto &kv : dict->dict)
    if (kv.first->kind == Value::Str && kv.first->s == key)
      return kv.second;
  return nullptr;
}

struct Loaded {
  const Zip &zip;
  V model, config;

  std::vector<double> tensor(const std::string &name, std::vector<int64_t> shape) const {
    V t = get(model, name);
    if (!t || t->kind != Value::Tensor)
      fail("missing tensor " + name);
    if (t->size != shape) {
      std::string got;
      for (auto s : t->size)
        got += std::to_string(s) + " ";
      fail("tensor " + name + " has shape " + got);
    }
    int64_t count = 1, expect = 1;
    for (int k = (int)shape.size() - 1; k >= 0; --k) {
      if (t->stride[k] != expect)
        fail("tensor " + name + " is not contiguous");
      expect *= shape[k];
    }
    count = expect;
    auto data = zip.find_suffix("/data/" + t->storage->key);
    std::vector<double> out(count);
    const std::string &dtype = t->storage->s;
    size_t width = dtype == "torch FloatStorage" ? 4 : dtype == "torch BoolStorage" ? 1 : 0;
    if (!width)
      fail("tensor " + name + " has unsupported storage " + dtype);
    if ((size_t)(t->offset + count) * width > data.second)
      fail("tensor " + name + " beyond its storage");
    for (int64_t k = 0; k < count; ++k) {
      const uint8_t *q = data.first + (t->offset + k) * width;
      if (width == 4) {
        float v;
        memcpy(&v, q, 4);
        if (!std::isfinite(v))
          fail("non-finite value in " + name);
        out[k] = v;
      } else {
        out[k] = *q;
      }
    }
    return out;
  }
  bool has(const std::string &name) const {
    V t = get(model, name);
    return t && t->kind == Value::Tensor;
  }
  int64_t config_int(const std::string &key, int64_t fallback) const {
    V v = get(config, key);
    return v && (v->kind == Value::Int || v->kind == Value::Bool) ? as_int(v) : fallback;
  }
};

} // namespace

namespace {

std::string spec_str(const V &spec, const std::string &key, const char *fallback = nullptr) {
  V v = get(spec, key);
  if (!v && fallback)
    return fallback;
  if (!v || v->kind != Value::Str)
    fail("spec." + key + " missing");
  return v->s;
}

CheckpointModel load_parallel(const Zip &zip, const V &root) {
  Loaded c{zip, get(root, "model"), get(root, "config")};
  V spec = get(root, "spec");
  if (!c.model || !spec)
    fail("model or spec missing");
  std::string kind = spec_str(spec, "model"), mixer = spec_str(spec, "mixer");
  V width = get(spec, "width");
  if (!width || width->kind != Value::Int)
    fail("spec.width missing");
  const int H = (int)as_int(width);
  LayerKind k;
  if (kind == "mingru")
    k = LayerKind::mingru;
  else if (kind == "igcifg")
    k = LayerKind::igcifg;
  else if (kind == "minlstm")
    k = LayerKind::minlstm;
  else
    fail("spec.model " + kind + " is not an input-gated model (CIFG runs load as cifg_lstm checkpoints)");
  auto f = std::make_unique<FloatModelFile>();
  std::string block = spec_str(spec, "block", "plain"), candidate = spec_str(spec, "candidate", "linear");
  if (block != "plain" && block != "residual" && block != "published")
    fail("unknown spec.block " + block);
  if (candidate != "linear" && candidate != "g" && candidate != "tanh")
    fail("unknown spec.candidate " + candidate);
  f->residual = block == "residual";
  f->published = block == "published";
  V op_flag = get(spec, "out_proj");
  f->out_proj = op_flag && (op_flag->kind == Value::Bool || op_flag->kind == Value::Int) && as_int(op_flag);
  f->candidate_g = candidate == "g";
  f->candidate_tanh = candidate == "tanh";
  // models.ParallelModel.mix: igcifg, mixer "none" and residual pass the output through
  f->mixer = (k == LayerKind::igcifg || mixer == "none" || f->residual || f->published) ? Mixer::none
             : mixer == "tanh"                            ? Mixer::tanh
             : mixer == "silu"                            ? Mixer::silu
                                                          : (fail("unknown mixer " + mixer), Mixer::none);
  f->hidden = H;
  while (c.has("layers." + std::to_string(f->n_layers) + ".proj.weight"))
    ++f->n_layers;
  if (!f->n_layers)
    fail("no layers");
  V hb = get(c.model, "head.bias");
  if (!hb || hb->kind != Value::Tensor || hb->size.size() != 1)
    fail("head.bias missing");
  f->n_phones = (int)hb->size[0];
  auto offset = c.tensor("offset", {40}), scale = c.tensor("scale", {40});
  f->affine_offset.assign(offset.begin(), offset.end());
  f->affine_scale.assign(scale.begin(), scale.end());
  auto dense = [&](const std::string &name, int rows, int cols) {
    FloatOp op;
    op.rows = op.w.rows = rows;
    op.cols = op.w.cols = cols;
    auto v = c.tensor(name, {rows, cols});
    op.w.f32.assign(v.begin(), v.end());
    return op;
  };
  const int G = gate_rows(k);
  auto vec = [&](const std::string &name, int n) {
    auto v = c.tensor(name, {n});
    return std::vector<float>(v.begin(), v.end());
  };
  if (f->published) {
    f->embed = dense("embed.weight", H, 80);
    f->embed_b = vec("embed.bias", H);
  }
  for (int l = 0; l < f->n_layers; ++l) {
    std::string p = "layers." + std::to_string(l) + ".proj.";
    FloatLayer layer;
    layer.kind = k;
    layer.inputs = l || f->published ? H : 80;
    layer.x = dense(p + "weight", G * H, layer.inputs);
    auto b = c.tensor(p + "bias", {G * H});
    layer.b_ih.assign(b.begin(), b.end());
    if (f->residual && l)
      layer.norm = vec("norms." + std::to_string(l - 1) + ".weight", H);
    if (f->published) {
      std::string n = std::to_string(l);
      layer.norm = vec("norms." + n + ".weight", H);
      layer.mlp_norm = vec("mlp_norms." + n + ".weight", H);
      if (f->out_proj) {
        layer.out = dense("outs." + n + ".weight", H, H);
        layer.out_b = vec("outs." + n + ".bias", H);
      }
      V up = get(c.model, "mlps." + n + ".up.bias");
      if (!up || up->kind != Value::Tensor || up->size.size() != 1)
        fail("mlps." + n + ".up.bias missing");
      const int E = (int)up->size[0];
      layer.up = dense("mlps." + n + ".up.weight", E, H);
      layer.up_b = vec("mlps." + n + ".up.bias", E);
      layer.down = dense("mlps." + n + ".down.weight", H, E);
      layer.down_b = vec("mlps." + n + ".down.bias", H);
    }
    f->layers.push_back(std::move(layer));
  }
  if (f->residual || f->published) {
    auto g = c.tensor("final_norm.weight", {H});
    f->final_norm.assign(g.begin(), g.end());
  }
  f->head = dense("head.weight", f->n_phones, H);
  auto bias = c.tensor("head.bias", {f->n_phones});
  f->head_bias.assign(bias.begin(), bias.end());
  CheckpointModel out;
  out.float_file = std::move(f);
  return out;
}

} // namespace

bool is_checkpoint(const std::vector<uint8_t> &bytes) {
  return bytes.size() >= 4 && le32(bytes.data()) == 0x04034b50;
}

CheckpointModel load_checkpoint(const std::vector<uint8_t> &bytes, const std::string &path) {
  try {
    Zip zip(bytes);
    auto pkl = zip.find_suffix("/data.pkl");
    V root = unpickle(pkl.first, pkl.second);
    V schema = get(root, "schema");
    if (schema && schema->s == "parallel-rnn-checkpoint-v1")
      return load_parallel(zip, root);
    if (!schema || schema->s != "cifg_lstm-checkpoint-v1")
      fail("not a cifg_lstm-checkpoint-v1 file");
    Loaded c{zip, get(root, "model"), get(root, "config")};
    if (!c.model || !c.config)
      fail("model or config missing");
    const int H = (int)c.config_int("hidden", 168);
    const int layers = (int)c.config_int("layers", 6);
    const bool floating = c.config_int("float_weights", 0) != 0;
    const int classes = (int)c.config_int("classes", 40);
    if (c.tensor("affine.frozen", {})[0] == 0)
      fail("the input affine is not frozen yet: no deployed model");
    auto offset = c.tensor("affine.offset", {40});
    auto scale = c.tensor("affine.frozen_scale", {40});
    CheckpointModel out;
    if (floating) {
      auto f = std::make_unique<FloatModelFile>();
      f->n_layers = layers;
      f->hidden = H;
      f->n_phones = classes;
      f->affine_offset.assign(offset.begin(), offset.end());
      f->affine_scale.assign(scale.begin(), scale.end());
      auto dense = [&](const std::string &name, int rows, int cols) {
        FloatOp op;
        op.rows = rows;
        op.cols = cols;
        op.w.rows = rows;
        op.w.cols = cols;
        auto v = c.tensor(name, {rows, cols});
        op.w.f32.assign(v.begin(), v.end());
        return op;
      };
      for (int l = 0; l < layers; ++l) {
        std::string p = "layers." + std::to_string(l) + ".";
        FloatLayer layer;
        layer.kind = LayerKind::split;
        layer.inputs = l ? H : 80;
        auto bi = c.tensor(p + "b_ih", {3 * H}), bh = c.tensor(p + "b_hh", {3 * H});
        layer.b_ih.assign(bi.begin(), bi.end());
        layer.b_hh.assign(bh.begin(), bh.end());
        layer.x = dense(p + "w_ih", 3 * H, layer.inputs);
        layer.h = dense(p + "w_hh", 3 * H, H);
        f->layers.push_back(std::move(layer));
      }
      f->head = dense("main_head.weight", classes, H);
      auto hb = c.tensor("main_head.bias", {classes});
      f->head_bias.assign(hb.begin(), hb.end());
      out.float_file = std::move(f);
    } else {
      auto f = std::make_unique<ModelFile>();
      f->n_groups = layers;
      f->hidden = H;
      f->n_phones = classes;
      for (int k = 0; k < 40; ++k) {
        double b = std::nearbyint(offset[k] * 1024), s = std::nearbyint(scale[k] * 1024);
        if (b < -32768 || b > 32767 || s < 1 || s > 32767)
          fail("affine does not fit int16");
        f->affine_bias.push_back((int16_t)b);
        f->affine_scale.push_back((int16_t)s);
      }
      auto codes = [&](const std::string &name, std::vector<int64_t> shape) {
        auto v = c.tensor(name, shape);
        std::vector<int8_t> q(v.size());
        for (size_t k = 0; k < v.size(); ++k) {
          if (v[k] < -1.0 || v[k] > 127.0 / 128.0)
            fail(name + " has a weight outside the int8 box; the engine will not clip it");
          q[k] = (int8_t)std::nearbyint(v[k] * 128.0);
        }
        return q;
      };
      auto matrix = [&](const std::string &w, const std::string &b, int in, int outd) {
        ModelMatrix m;
        m.in_dim = in;
        m.out_dim = outd;
        m.w = codes(w, {outd, in});
        m.bias = codes(b, {outd});
        m.w16.assign(m.w.begin(), m.w.end());
        return m;
      };
      for (int l = 0; l < layers; ++l) {
        std::string p = "layers." + std::to_string(l) + ".";
        f->wx.push_back(matrix(p + "w_ih", p + "b_ih", l ? H : 80, 3 * H));
        f->wh.push_back(matrix(p + "w_hh", p + "b_hh", H, 3 * H));
      }
      f->wout = matrix("main_head.weight", "main_head.bias", H, classes);
      out.int_file = std::move(f);
    }
    return out;
  } catch (const std::out_of_range &) {
    throw std::runtime_error(path + ": checkpoint pickle refers to an unknown memo entry");
  } catch (const std::runtime_error &e) {
    throw std::runtime_error(path + ": " + e.what());
  }
}

} // namespace kws::detail
