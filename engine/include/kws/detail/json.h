#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kws::detail::json {

class Value {
public:
  enum class Kind { Null, Bool, Number, String, Array, Object };

  Kind kind() const { return kind_; }
  bool is_null() const { return kind_ == Kind::Null; }
  bool is_object() const { return kind_ == Kind::Object; }
  bool is_array() const { return kind_ == Kind::Array; }
  bool as_bool() const;
  double as_number() const;
  const std::string &as_string() const;

  bool has(const std::string &key) const;
  const Value &get(const std::string &key) const;

  const std::vector<Value> &items() const;
  size_t size() const;

  Kind kind_ = Kind::Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::vector<Value> arr_;
  std::vector<std::pair<std::string, Value>> obj_;
};

Value parse(const std::vector<uint8_t> &bytes);

std::string escape(const std::string &s);

} // namespace kws::detail::json
