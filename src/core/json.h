// Minimal JSON value, parser and writer (enough for scene and project files; no external dependency).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double n) : type_(Type::Number), number_(n) {}
    Value(float n) : Value((double)n) {}
    Value(int n) : Value((double)n) {}
    Value(unsigned n) : Value((double)n) {}
    Value(const char* s) : type_(Type::String), string_(s) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }
    static Value vec3(const float v[3]) { Value a = array(); for (int i = 0; i < 3; ++i) a.push(v[i]); return a; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }

    // Reading never throws: a missing key or wrong type yields the default
    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double asNumber(double def = 0.0) const { return type_ == Type::Number ? number_ : def; }
    float asFloat(float def = 0.f) const { return type_ == Type::Number ? (float)number_ : def; }
    int asInt(int def = 0) const { return type_ == Type::Number ? (int)number_ : def; }
    const std::string& asString() const;
    void readVec3(float out[3]) const;   // leaves `out` unchanged unless this is an array of 3 numbers

    bool has(const std::string& key) const;
    const Value& operator[](const std::string& key) const;   // null value if missing
    const Value& operator[](size_t index) const;
    size_t size() const { return type_ == Type::Array ? array_.size() : type_ == Type::Object ? object_.size() : 0; }
    const std::vector<Value>& elements() const { return array_; }
    const std::vector<std::pair<std::string, Value>>& members() const { return object_; }

    // Building (turns a null value into an array / object)
    Value& set(const std::string& key, Value v);   // keeps insertion order
    Value& push(Value v);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

std::string write(const Value& v);   // pretty-printed; short arrays of numbers stay on one line
bool parse(const std::string& text, Value& out, std::string& error);

}  // namespace json
