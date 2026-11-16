// Tiny JSON value type (enough for the greetd IPC protocol)
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fld {

class Json {
public:
  enum Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool b) : type_(Bool), b_(b) {}
  Json(int n) : type_(Number), n_(n) {}
  Json(double n) : type_(Number), n_(n) {}
  Json(const char *s) : type_(String), s_(s) {}
  Json(const std::string &s) : type_(String), s_(s) {}
  static Json array() { Json j; j.type_ = Array; return j; }
  static Json object() { Json j; j.type_ = Object; return j; }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Null; }
  bool is_string() const { return type_ == String; }
  bool is_object() const { return type_ == Object; }
  bool is_array() const { return type_ == Array; }

  bool as_bool() const { return type_ == Bool && b_; }
  double as_number() const { return type_ == Number ? n_ : 0; }
  const std::string &as_string() const { return s_; }
  const std::vector<Json> &items() const { return a_; }
  const std::map<std::string, Json> &members() const { return o_; }

  // object access: returns a null value when missing
  const Json &operator[](const std::string &key) const;
  Json &set(const std::string &key, const Json &v);
  Json &push(const Json &v);
  bool has(const std::string &key) const { return o_.count(key) > 0; }

  std::string dump() const;
  // Returns false on syntax error.
  static bool parse(const std::string &text, Json &out, std::string *err = nullptr);

private:
  Type type_ = Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::vector<Json> a_;
  std::map<std::string, Json> o_;
  void dump_to(std::string &out) const;
};

} // namespace fld
