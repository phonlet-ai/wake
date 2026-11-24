#include "json.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace kws::detail::json {

namespace {

struct Parser {
  const char *p;
  const char *end;
  const char *start;

  [[noreturn]] void fail(const char *why) {
    throw std::runtime_error(std::string("JSON: ") + why + " at byte " +
                             std::to_string((size_t)(p - start)));
  }
  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
      ++p;
  }
  char peek() {
    if (p >= end)
      fail("unexpected end");
    return *p;
  }
  void expect(char c) {
    if (p >= end || *p != c)
      fail("unexpected character");
    ++p;
  }

  Value value() {
    ws();
    char c = peek();
    if (c == '{')
      return object();
    if (c == '[')
      return array();
    if (c == '"') {
      Value v;
      v.kind_ = Value::Kind::String;
      v.s_ = string();
      return v;
    }
    if (c == 't' || c == 'f')
      return boolean();
    if (c == 'n') {
      literal("null");
      return Value{};
    }
    return number();
  }

  Value object() {
    expect('{');
    Value v;
    v.kind_ = Value::Kind::Object;
    ws();
    if (peek() == '}') {
      ++p;
      return v;
    }
    for (;;) {
      ws();
      std::string key = string();
      ws();
      expect(':');
      v.obj_.emplace_back(std::move(key), value());
      ws();
      char c = peek();
      if (c == ',') {
        ++p;
        continue;
      }
      if (c == '}') {
        ++p;
        break;
      }
      fail("expected , or }");
    }
    return v;
  }

  Value array() {
    expect('[');
    Value v;
    v.kind_ = Value::Kind::Array;
    ws();
    if (peek() == ']') {
      ++p;
      return v;
    }
    for (;;) {
      v.arr_.push_back(value());
      ws();
      char c = peek();
      if (c == ',') {
        ++p;
        continue;
      }
      if (c == ']') {
        ++p;
        break;
      }
      fail("expected , or ]");
    }
    return v;
  }

  std::string string() {
    expect('"');
    std::string out;
    for (;;) {
      if (p >= end)
        fail("unterminated string");
      char c = *p++;
      if (c == '"')
        break;
      if (c == '\\') {
        if (p >= end)
          fail("bad escape");
        char e = *p++;
        switch (e) {
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case '/':
          out += '/';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          if (end - p < 4)
            fail("bad \\u");
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            char h = *p++;
            cp <<= 4;
            if (h >= '0' && h <= '9')
              cp |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f')
              cp |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
              cp |= (unsigned)(h - 'A' + 10);
            else
              fail("bad hex digit");
          }

          if (cp < 0x80)
            out += (char)cp;
          else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
          } else {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
          }
          break;
        }
        default:
          fail("bad escape");
        }
      } else {
        out += c;
      }
    }
    return out;
  }

  Value boolean() {
    Value v;
    v.kind_ = Value::Kind::Bool;
    if (p + 4 <= end && !strncmp(p, "true", 4)) {
      v.b_ = true;
      p += 4;
      return v;
    }
    if (p + 5 <= end && !strncmp(p, "false", 5)) {
      v.b_ = false;
      p += 5;
      return v;
    }
    fail("bad literal");
  }

  void literal(const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(end - p) < n || strncmp(p, lit, n) != 0)
      fail("bad literal");
    p += n;
  }

  Value number() {
    const char *start = p;
    if (p < end && *p == '-')
      ++p;
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' ||
                       *p == 'E' || *p == '+' || *p == '-'))
      ++p;
    if (p == start)
      fail("bad number");
    std::string tok(start, p);
    Value v;
    v.kind_ = Value::Kind::Number;
    v.n_ = strtod(tok.c_str(), nullptr);
    return v;
  }
};

} // namespace

bool Value::as_bool() const {
  if (kind_ != Kind::Bool)
    throw std::runtime_error("JSON: expected bool");
  return b_;
}
double Value::as_number() const {
  if (kind_ != Kind::Number)
    throw std::runtime_error("JSON: expected number");
  return n_;
}
const std::string &Value::as_string() const {
  if (kind_ != Kind::String)
    throw std::runtime_error("JSON: expected string");
  return s_;
}
bool Value::has(const std::string &key) const {
  if (kind_ != Kind::Object)
    return false;
  for (const auto &kv : obj_)
    if (kv.first == key)
      return true;
  return false;
}
const Value &Value::get(const std::string &key) const {
  if (kind_ != Kind::Object)
    throw std::runtime_error("JSON: expected object");
  for (const auto &kv : obj_)
    if (kv.first == key)
      return kv.second;
  throw std::runtime_error("JSON: missing key '" + key + "'");
}
const std::vector<Value> &Value::items() const {
  if (kind_ != Kind::Array)
    throw std::runtime_error("JSON: expected array");
  return arr_;
}
size_t Value::size() const { return arr_.size(); }

Value parse(const std::vector<uint8_t> &bytes) {
  const char *s = (const char *)bytes.data();
  Parser ps{s, s + bytes.size(), s};
  Value v = ps.value();
  ps.ws();
  if (ps.p != ps.end)
    ps.fail("trailing content");
  return v;
}

std::string escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if ((unsigned char)c < 0x20) {
        char buf[8];
        snprintf(buf, sizeof buf, "\\u%04x", c);
        out += buf;
      } else {
        out += c;
      }
    }
  }
  return out;
}

} // namespace kws::detail::json
