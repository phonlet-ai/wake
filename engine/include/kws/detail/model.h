#pragma once
#include "kernels.h"
#include <cstdint>
#include <string>
#include <vector>

namespace kws::detail {

struct ModelMatrix {
  int in_dim = 0, out_dim = 0;
  std::vector<int8_t> bias;
  std::vector<int8_t> w;
  std::vector<int16_t> w16;
};

struct ModelFile {
  std::vector<int16_t> affine_bias, affine_scale;
  std::vector<ModelMatrix> wx, wh;
  ModelMatrix wout;
  int n_groups = 0;
  int hidden = 0;
  int n_phones = 0;
};

std::vector<uint8_t> read_file(const std::string &path);

struct Reader {
  const uint8_t *b;
  size_t n, off = 0;
  void need(size_t k) const;
  uint32_t u32();
  uint16_t u16();
  uint8_t u8();
  void i16n(int16_t *dst, size_t k);
};

ModelFile load_model_body(Reader &r, int n_groups, const std::string &path);

ModelFile load_model(const std::string &path);
ModelFile load_model(const std::vector<uint8_t> &bytes, const std::string &label);

void write_model(const std::string &path, const std::vector<uint8_t> &body,
                 int n_layers, int hidden, int n_phones);

class Model {
public:
  explicit Model(const ModelFile &file);
  void reset();

  void step(const int16_t *mel80, int32_t *post40, int16_t *affine80 = nullptr);

  int hidden() const { return file_.hidden; }
  int n_groups() const { return file_.n_groups; }

#ifdef KWS_DEV

  struct LayerTap {
    std::vector<int16_t> a, bt, c;
    std::vector<int16_t> h;
  };
  const std::vector<LayerTap> &taps() const { return taps_; }
#endif

private:
  const ModelFile &file_;
  Dot dot_;
  std::vector<int16_t> v_;
  std::vector<int16_t> y_, cstate_;
  std::vector<int16_t> g_;
  std::vector<int32_t> e_;

  static int32_t sigmoid_(int32_t v);
  static int32_t tanh_(int32_t v);
  void matmul_(int16_t *dst, const int16_t *x, const ModelMatrix &m,
               bool accumulate) const;

#ifdef KWS_DEV
  std::vector<LayerTap> taps_;
#endif
};

} // namespace kws::detail
