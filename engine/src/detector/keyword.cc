#include "keyword.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "json.h"
#include "runtime_contract.h"

namespace kws::detail {

namespace {

struct BinReader {
  std::vector<uint8_t> b;
  size_t off = 0;
  void need(size_t k) const {
    if (off + k > b.size())
      throw std::runtime_error("keyword file truncated");
  }
  uint8_t u8() {
    need(1);
    return b[off++];
  }
  uint16_t u16() {
    need(2);
    uint16_t v = (uint16_t)(b[off] | b[off + 1] << 8);
    off += 2;
    return v;
  }
  float f32() {
    need(4);
    float v;
    memcpy(&v, b.data() + off, 4);
    off += 4;
    return v;
  }
};

std::vector<uint8_t> slurp(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    throw std::runtime_error("cannot open " + path);
  std::vector<uint8_t> buf;
  uint8_t chunk[1 << 12];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    buf.insert(buf.end(), chunk, chunk + n);
  fclose(f);
  return buf;
}

} // namespace

KeywordSpec load_keyword_bin(const std::string &path) {
  BinReader r{slurp(path)};
  r.need(4);
  if (memcmp(r.b.data(), KWS_KW_MAGIC, 4) != 0)
    throw std::runtime_error(path + ": bad keyword magic");
  r.off = 4;
  uint16_t version = r.u16();
  if (version != 1)
    throw std::runtime_error(path + ": keyword version " +
                             std::to_string(version));
  KeywordSpec s;
  uint8_t flags = r.u8();
  s.endpoint = flags & 1;
  uint8_t name_len = r.u8();
  r.need(name_len);
  s.name.assign((const char *)r.b.data() + r.off, name_len);
  r.off += name_len;
  uint16_t n_var = r.u16();
  if (n_var == 0 || n_var > 64)
    throw std::runtime_error(path + ": bad pronunciation count");
  s.pronunciations.resize(n_var);
  for (auto &v : s.pronunciations) {
    uint16_t len = r.u16();
    if (len == 0 || len > 64)
      throw std::runtime_error(path + ": bad variant length");
    v.resize(len);
    for (auto &id : v) {
      id = r.u16();
      if (id < 1 || id > 999)
        throw std::runtime_error(path + ": bad phone id");
    }
  }
  s.threshold_loose = r.f32();
  s.threshold_strict = r.f32();
  return s;
}

KeywordSpec load_keyword_json(const std::string &path) {
  json::Value root = json::parse(slurp(path));
  if (!root.is_object())
    throw std::runtime_error(path + ": not a JSON object");
  KeywordSpec s;
  s.name = root.get("name").as_string();
  const json::Value &ep = root.get("endpoint");
  if (!ep.is_null())
    s.endpoint = ep.as_bool();

  s.threshold_loose = (float)root.get("threshold_loose").as_number();
  s.threshold_strict = (float)root.get("threshold_strict").as_number();
  const json::Value &vars = root.get("pronunciations");
  if (!vars.is_array() || vars.size() == 0)
    throw std::runtime_error(path + ": pronunciations must be a non-empty array");
  for (const auto &v : vars.items()) {
    if (!v.is_array())
      throw std::runtime_error(path + ": variant must be an array");
    std::vector<int> ids;
    for (const auto &id : v.items()) {
      int n = (int)id.as_number();

      if (n < 1 || n > 999)
        throw std::runtime_error(path + ": bad phone id " + std::to_string(n));
      ids.push_back(n);
    }
    if (ids.empty())
      throw std::runtime_error(path + ": empty variant");
    s.pronunciations.push_back(std::move(ids));
  }
  return s;
}

KeywordSpec load_keyword(const std::string &path) {
  std::vector<uint8_t> b = slurp(path);
  if (b.size() >= 4 && memcmp(b.data(), KWS_KW_MAGIC, 4) == 0)
    return load_keyword_bin(path);
  for (uint8_t c : b)
    if (c == '{')
      return load_keyword_json(path);
    else if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      break;
  throw std::runtime_error(path + ": not a " KWS_KW_MAGIC
                                  " binary or JSON keyword file");
}

void save_keyword_bin(const KeywordSpec &s, const std::string &path) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    throw std::runtime_error("cannot write " + path);
  auto w16 = [&](uint16_t v) {
    fputc(v & 0xff, f);
    fputc(v >> 8, f);
  };
  fwrite(KWS_KW_MAGIC, 1, 4, f);
  w16(1);
  fputc(s.endpoint ? 1 : 0, f);
  if (s.name.size() > 255)
    throw std::runtime_error("keyword name too long");
  fputc((int)s.name.size(), f);
  fwrite(s.name.data(), 1, s.name.size(), f);
  w16((uint16_t)s.pronunciations.size());
  for (const auto &v : s.pronunciations) {
    w16((uint16_t)v.size());
    for (int id : v)
      w16((uint16_t)id);
  }
  fwrite(&s.threshold_loose, 4, 1, f);
  fwrite(&s.threshold_strict, 4, 1, f);
  fclose(f);
}

void save_keyword_json(const KeywordSpec &s, const std::string &path) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f)
    throw std::runtime_error("cannot write " + path);
  fprintf(f, "{\n");
  fprintf(f, "  \"format\": \"" KWS_KW_FORMAT "\",\n");
  fprintf(f, "  \"version\": 1,\n");
  fprintf(f, "  \"name\": \"%s\",\n", json::escape(s.name).c_str());
  fprintf(f, "  \"endpoint\": %s,\n", s.endpoint ? "true" : "false");

  fprintf(f, "  \"threshold_loose\": %.9g,\n", (double)s.threshold_loose);
  fprintf(f, "  \"threshold_strict\": %.9g,\n", (double)s.threshold_strict);
  fprintf(f, "  \"pronunciations\": [");
  for (size_t i = 0; i < s.pronunciations.size(); ++i) {
    fprintf(f, "%s\n    [", i ? "," : "");
    for (size_t j = 0; j < s.pronunciations[i].size(); ++j)
      fprintf(f, "%s%d", j ? ", " : "", s.pronunciations[i][j]);
    fprintf(f, "]");
  }
  fprintf(f, "\n  ]\n}\n");
  fclose(f);
}

int16_t threshold_for(const KeywordSpec &s, double sensitivity) {
  double t = (s.threshold_strict - (s.threshold_strict - (double)s.threshold_loose) * sensitivity) * 1024.0;
  if (t > 32767.0)
    t = 32767.0;
  if (t < -32768.0)
    t = -32768.0;
  return (int16_t)(int)t;
}

} // namespace kws::detail
