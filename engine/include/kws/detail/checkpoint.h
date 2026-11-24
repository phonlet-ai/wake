#pragma once
// Direct loading of cifg_lstm training checkpoints (src/io/checkpoint.cc).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "float_model.h"
#include "model.h"

namespace kws::detail {

struct CheckpointModel {
  std::unique_ptr<ModelFile> int_file;        // boxed int8 checkpoint
  std::unique_ptr<FloatModelFile> float_file; // float_weights checkpoint
};

bool is_checkpoint(const std::vector<uint8_t> &bytes);
CheckpointModel load_checkpoint(const std::vector<uint8_t> &bytes, const std::string &path);

} // namespace kws::detail
