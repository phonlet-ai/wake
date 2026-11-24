#include "wav.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace kws::detail {

namespace {

uint32_t rd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

std::vector<uint8_t> read_file(const std::string &path) {
  FILE *f = stdin;
  bool is_stdin = path == "-" || path.empty();
  if (!is_stdin) {
    f = fopen(path.c_str(), "rb");
    if (!f)
      throw std::runtime_error("cannot open " + path);
  }
  std::vector<uint8_t> buf;
  uint8_t chunk[1 << 16];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    buf.insert(buf.end(), chunk, chunk + n);
  if (!is_stdin)
    fclose(f);
  return buf;
}

} // namespace

Wav load_wav(const std::string &path) {
  std::vector<uint8_t> b = read_file(path);
  if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) != 0 ||
      memcmp(b.data() + 8, "WAVE", 4) != 0)
    throw std::runtime_error(path + ": not a RIFF/WAVE file");

  Wav w;
  bool have_fmt = false, have_data = false;
  size_t off = 12;
  while (off + 8 <= b.size()) {
    const uint8_t *id = b.data() + off;
    uint32_t sz = rd_u32(b.data() + off + 4);
    size_t body = off + 8;
    if (body + sz > b.size())
      throw std::runtime_error(path + ": truncated WAV chunk");
    if (!memcmp(id, "fmt ", 4)) {
      if (sz < 16)
        throw std::runtime_error(path + ": fmt chunk too small");
      uint16_t audio_format = rd_u16(b.data() + body);
      uint16_t channels = rd_u16(b.data() + body + 2);
      w.sample_rate = (int)rd_u32(b.data() + body + 4);
      uint16_t bits = rd_u16(b.data() + body + 14);
      if (audio_format != 1 || channels != 1 || bits != 16)
        throw std::runtime_error(path + ": want PCM16 mono, got format " +
                                 std::to_string(audio_format) + " ch " +
                                 std::to_string(channels) + " bits " +
                                 std::to_string(bits));
      have_fmt = true;
    } else if (!memcmp(id, "data", 4)) {
      if (sz % 2)
        throw std::runtime_error(path + ": partial PCM16 sample");
      size_t n_samples = sz / 2;
      w.pcm.resize(n_samples);

      if (n_samples)
        memcpy(w.pcm.data(), b.data() + body, n_samples * 2);
      have_data = true;
    }
    off = body + sz + (sz & 1);
  }
  if (!have_fmt || !have_data)
    throw std::runtime_error(path + ": missing fmt/data");
  if (w.sample_rate != 16000)
    throw std::runtime_error(path + ": want 16 kHz, got " +
                             std::to_string(w.sample_rate));
  return w;
}

std::vector<int16_t> load_raw_pcm(const std::string &path) {
  std::vector<uint8_t> b = read_file(path);
  std::vector<int16_t> pcm(b.size() / 2);
  if (!pcm.empty())
    memcpy(pcm.data(), b.data(), pcm.size() * 2);
  return pcm;
}

} // namespace kws::detail
