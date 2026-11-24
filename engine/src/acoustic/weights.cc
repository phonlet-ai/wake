#include "checkpoint.h"
#include "float_model.h"
#include "internal.h"
#include "kws/runtime.hpp"
#include "model.h"
#include <sstream>
#include "runtime_tables.h"
namespace kws::detail {
void validate_weights(const ModelFile &file, const LanguagePreset &language) {
  // engine-multi: any int8 width the loader accepts (1..512), six layers.
  require(file.n_groups == 6 && file.hidden >= 1 && file.hidden <= 512,
          "expected six recurrent layers of width 1..512");
  const int H = file.hidden;
  require(file.n_phones == language.classes(),
          "model output width does not match selected language");
  require(file.affine_bias.size() == 40 && file.affine_scale.size() == 40,
          "expected initialized 40-band affine");
  for (auto scale : file.affine_scale)
    require(scale > 0, "nonpositive affine scale");
  require(file.wx.size() == 6 && file.wh.size() == 6, "invalid layer count");
  size_t values = 80;
  auto check = [&](const ModelMatrix &m, int input, int output) {
    require(m.in_dim == input && m.out_dim == output &&
                m.bias.size() == size_t(output) &&
                m.w.size() == size_t(input * output) &&
                m.w16.size() == m.w.size(),
            "invalid matrix shape");
    values += m.bias.size() + m.w.size();
  };
  for (int layer = 0; layer < 6; ++layer) {
    check(file.wx[layer], layer ? H : 80, 3 * H);
    check(file.wh[layer], H, 3 * H);
  }
  check(file.wout, H, language.classes());
  require(values == size_t(80 + 3 * H * (80 + 1) + 3 * H * (H + 1) +
                           5 * (3 * H * (H + 1) + 3 * H * (H + 1)) +
                           language.classes() * (H + 1)),
          "invalid parameter count");
}
} // namespace kws::detail
namespace kws {
ModelWeights::ModelWeights(std::unique_ptr<detail::ModelFile> file,
                           const LanguagePreset &language)
    : parameters_(std::move(file)), language_(language) {}
ModelWeights::~ModelWeights() = default;
int ModelWeights::classes() const noexcept {
  return float_ ? float_->n_phones : parameters_->n_phones;
}
const LanguagePreset &ModelWeights::language() const noexcept {
  return language_;
}
ModelWeights::ModelWeights(std::unique_ptr<detail::FloatModelFile> file,
                           const LanguagePreset &language)
    : float_(std::move(file)), language_(language) {}
const detail::ModelFile &ModelWeights::parameters() const {
  detail::require(bool(parameters_), "a float-container model has no int8 parameters");
  return *parameters_;
}
const detail::FloatModelFile &ModelWeights::float_parameters() const {
  detail::require(bool(float_), "not a float-container model");
  return *float_;
}
namespace {
class IntAcoustic final : public detail::Acoustic {
public:
  explicit IntAcoustic(const detail::ModelFile &file) : model_(file) {}
  void reset() override { model_.reset(); }
  void step(const int16_t *mel, int32_t *post) override { model_.step(mel, post); }

private:
  detail::Model model_;
};
} // namespace
namespace {
detail::FloatAcousticFactory float_acoustic_factory = nullptr;
} // namespace
void detail::set_float_acoustic_factory(FloatAcousticFactory factory) {
  float_acoustic_factory = factory;
}
std::unique_ptr<detail::Acoustic> ModelWeights::make_acoustic() const {
  if (float_ && float_acoustic_factory)
    if (auto platform = float_acoustic_factory(*float_))
      return platform;
  if (float_)
    return std::make_unique<detail::FloatModel>(*float_);
  return std::make_unique<IntAcoustic>(*parameters_);
}
std::string ModelWeights::info_json() const {
  std::ostringstream o;
  if (float_) {
    const auto &f = *float_;
    o << "{\"format\":\"phonletwake-float\",\"layers\":" << f.n_layers << ",\"hidden\":" << f.hidden
      << ",\"classes\":" << f.n_phones << ",\"parameters\":" << f.parameters()
      << ",\"tensor_bytes\":" << f.tensor_bytes() << ",\"kernel\":\""
      << detail::float_kernel() << "\",\"layer_maps\":[";
    auto op = [&](const detail::FloatOp &x) {
      const char *st[] = {"f32", "f16", "i8"};
      const auto &m = x.lowrank ? x.a : x.w;
      o << "{\"rank\":" << (x.lowrank ? x.rank : 0) << ",\"storage\":\""
        << st[(int)m.storage] << "\"}";
    };
    for (size_t l = 0; l < f.layers.size(); ++l) {
      const auto &L = f.layers[l];
      o << (l ? "," : "") << "{\"kind\":\""
        << (L.kind == detail::LayerKind::joint ? "joint" : "split") << "\",";
      if (L.kind == detail::LayerKind::joint) {
        o << "\"u\":";
        op(L.u);
      } else {
        o << "\"x\":";
        op(L.x);
        o << ",\"h\":";
        op(L.h);
      }
      o << "}";
    }
    o << "],\"head\":";
    op(f.head);
    o << "}";
  } else {
    const auto &f = *parameters_;
    o << "{\"format\":\"phonletwake-quantized\",\"layers\":" << f.n_groups << ",\"hidden\":" << f.hidden
      << ",\"classes\":" << f.n_phones << ",\"kernel\":\"" << detail::selected_kernel()
      << "\"}";
  }
  return o.str();
}
std::shared_ptr<const ModelWeights>
ModelWeights::load(const std::string &path, const std::string &code) {
  return from_bytes(detail::read_file(path), code);
}
std::shared_ptr<const ModelWeights>
ModelWeights::from_bytes(const std::vector<uint8_t> &bytes, const std::string &code) {
  const auto &language = language_preset(code);
  const std::string path = "model bytes";
  if (detail::is_checkpoint(bytes)) {
    auto loaded = detail::load_checkpoint(bytes, path);
    if (loaded.float_file) {
      detail::require(loaded.float_file->n_phones == language.classes(),
                      "model output width does not match selected language");
      return std::shared_ptr<const ModelWeights>(
          new ModelWeights(std::move(loaded.float_file), language));
    }
    detail::validate_weights(*loaded.int_file, language);
    return std::shared_ptr<const ModelWeights>(
        new ModelWeights(std::move(loaded.int_file), language));
  }
  if (detail::is_float_container(bytes)) {
    auto f = std::make_unique<detail::FloatModelFile>(detail::load_float_model(bytes, path));
    detail::require(f->n_phones == language.classes(),
                    "model output width does not match selected language");
    return std::shared_ptr<const ModelWeights>(new ModelWeights(std::move(f), language));
  }
  auto file = std::make_unique<detail::ModelFile>(detail::load_model(bytes, path));
  detail::validate_weights(*file, language);
  return std::shared_ptr<const ModelWeights>(
      new ModelWeights(std::move(file), language));
}
std::shared_ptr<const ModelWeights>
ModelWeights::from_tensors(uint32_t frontend, const int16_t *affine,
                           size_t affine_count, const int8_t *dense,
                           size_t dense_count, const std::string &code) {
  const auto &language = language_preset(code);
  detail::require(frontend == detail::kFrontendId,
                  "checkpoint frontend mismatch");
  detail::require(affine && affine_count == 80, "expected 80 affine values");
  detail::require(dense &&
                      dense_count == 977760 + size_t(language.classes()) * 169,
                  "dense tensor count does not match selected language");
  auto file = std::make_unique<detail::ModelFile>();
  file->n_groups = 6;
  file->hidden = 168;
  file->n_phones = language.classes();
  file->affine_bias.assign(affine, affine + 40);
  file->affine_scale.assign(affine + 40, affine + 80);
  size_t offset = 0;
  auto matrix = [&](int input, int output) {
    detail::ModelMatrix m;
    m.in_dim = input;
    m.out_dim = output;
    m.bias.assign(dense + offset, dense + offset + output);
    offset += output;
    m.w.assign(dense + offset, dense + offset + input * output);
    offset += input * output;
    m.w16.assign(m.w.begin(), m.w.end());
    return m;
  };
  for (int layer = 0; layer < 6; ++layer) {
    file->wx.push_back(matrix(layer ? 168 : 80, 504));
    file->wh.push_back(matrix(168, 504));
  }
  file->wout = matrix(168, language.classes());
  detail::validate_weights(*file, language);
  return std::shared_ptr<const ModelWeights>(
      new ModelWeights(std::move(file), language));
}
} // namespace kws
