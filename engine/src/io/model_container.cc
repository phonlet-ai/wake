#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "container.h"
#include "model.h"
#include "runtime_contract.h"

namespace kws::detail {

namespace {

std::string hex32(uint32_t v) {
  char b[16];
  snprintf(b, sizeof b, "0x%08" PRIx32, v);
  return b;
}

void expect(int got, int want, const char *what, const std::string &path) {
  if (got != want)
    throw std::runtime_error(path + ": header declares " + what + " = " +
                             std::to_string(want) + ", body has " +
                             std::to_string(got));
}

void put_u32(std::vector<uint8_t> &out, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back((v >> (8 * i)) & 0xff);
}

void put_u16(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(v & 0xff);
  out.push_back(v >> 8);
}

} // namespace

ModelFile load_model(const std::string &path) {
  return load_model(read_file(path), path);
}

ModelFile load_model(const std::vector<uint8_t> &buf, const std::string &path) {
  if (container_kind(buf) != kContainerKindQuantized)
    throw std::runtime_error(path + ": expected a phonletwake quantized container");

  Reader r{buf.data(), buf.size(), kContainerMagicSize};
  if (r.u32() != kContainerKindQuantized)
    throw std::runtime_error(path + ": wrong container kind");
  uint32_t version = r.u32();
  if (version != kContainerVersion)
    throw std::runtime_error(path + ": container version " + std::to_string(version));
  uint32_t header_len = r.u32();
  uint32_t file_fid = r.u32();
  if (file_fid != kFrontendId)
    throw std::runtime_error(
        path + ": trained against frontend " + hex32(file_fid) +
        ", but this engine computes frontend " + hex32(kFrontendId) +
        ". The features this model expects are not the features it would be"
        " given; rebuild the mel cache and retrain, or run the engine this"
        " model was built for.");

  int n_layers = (int)r.u16();
  int hidden = (int)r.u16();
  int n_phones = (int)r.u16();
  int n_mels = (int)r.u16();
  int stack = (int)r.u16();
  (void)r.u16();

  if (n_mels * stack != 80)
    throw std::runtime_error(path + ": declares " + std::to_string(n_mels) +
                             " mels x " + std::to_string(stack) +
                             " stacked, but the model step is fed 80 values");
  if (header_len < kContainerHeaderReserve || header_len > buf.size())
    throw std::runtime_error(path + ": implausible header length " +
                             std::to_string(header_len));
  r.off = header_len;

  ModelFile f = load_model_body(r, n_layers, path);
  expect(f.hidden, hidden, "hidden", path);
  expect(f.n_phones, n_phones, "n_phones", path);
  return f;
}

void write_model(const std::string &path, const std::vector<uint8_t> &body,
                 int n_layers, int hidden, int n_phones) {
  std::vector<uint8_t> out(kContainerMagic, kContainerMagic + kContainerMagicSize);
  put_u32(out, kContainerKindQuantized);
  put_u32(out, kContainerVersion);
  put_u32(out, kContainerHeaderReserve);
  put_u32(out, kFrontendId);
  put_u16(out, (uint16_t)n_layers);
  put_u16(out, (uint16_t)hidden);
  put_u16(out, (uint16_t)n_phones);
  put_u16(out, 40);
  put_u16(out, 2);
  put_u16(out, 0);
  out.push_back(0); // pad the fixed header to kContainerHeaderReserve
  out.insert(out.end(), body.begin(), body.end());
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    throw std::runtime_error("cannot write " + path);
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
}

} // namespace kws::detail
