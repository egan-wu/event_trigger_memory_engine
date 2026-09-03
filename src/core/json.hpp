#pragma once
// Minimal dependency-free JSON reader/writer, sufficient for flat/nested
// config and report schemas. Not a full RFC 8259 implementation (no \uXXXX
// surrogate pair decoding), but handles standard object/array/string/number/
// bool/null documents.
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ddrtiming::json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>; // insertion-ordered

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() : type_(Type::Null) {}
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double d) : type_(Type::Number), num_(d) {}
    Value(int i) : type_(Type::Number), num_(static_cast<double>(i)) {}
    Value(int64_t i) : type_(Type::Number), num_(static_cast<double>(i)) {}
    Value(uint64_t i) : type_(Type::Number), num_(static_cast<double>(i)) {}
    Value(const char* s) : type_(Type::String), str_(s) {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Value(Array a) : type_(Type::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : type_(Type::Object), obj_(std::make_shared<Object>(std::move(o))) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }

    bool as_bool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double as_double(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    int64_t as_int(int64_t def = 0) const { return type_ == Type::Number ? static_cast<int64_t>(num_) : def; }
    std::string as_string(const std::string& def = std::string()) const { return type_ == Type::String ? str_ : def; }

    bool contains(const std::string& key) const {
        if (type_ != Type::Object || !obj_) return false;
        for (auto& kv : *obj_) if (kv.first == key) return true;
        return false;
    }

    const Value& operator[](const std::string& key) const {
        static const Value null_val;
        if (type_ != Type::Object || !obj_) return null_val;
        for (auto& kv : *obj_) if (kv.first == key) return kv.second;
        return null_val;
    }

    const Value& at(size_t idx) const {
        static const Value null_val;
        if (type_ != Type::Array || !arr_ || idx >= arr_->size()) return null_val;
        return (*arr_)[idx];
    }

    size_t size() const {
        if (type_ == Type::Array && arr_) return arr_->size();
        if (type_ == Type::Object && obj_) return obj_->size();
        return 0;
    }

    const Array& array_items() const { static const Array e; return (type_ == Type::Array && arr_) ? *arr_ : e; }
    const Object& object_items() const { static const Object e; return (type_ == Type::Object && obj_) ? *obj_ : e; }

    double get_num(const std::string& key, double def) const { return contains(key) ? (*this)[key].as_double(def) : def; }
    int64_t get_int(const std::string& key, int64_t def) const { return contains(key) ? (*this)[key].as_int(def) : def; }
    std::string get_str(const std::string& key, const std::string& def) const { return contains(key) ? (*this)[key].as_string(def) : def; }

    std::string dump(int indent = 2) const;

    static Value make_array() { return Value(Array{}); }
    static Value make_object() { return Value(Object{}); }
    void push_back(Value v) { if (!arr_) arr_ = std::make_shared<Array>(); type_ = Type::Array; arr_->push_back(std::move(v)); }
    void set(const std::string& key, Value v) {
        if (!obj_) obj_ = std::make_shared<Object>();
        type_ = Type::Object;
        for (auto& kv : *obj_) { if (kv.first == key) { kv.second = std::move(v); return; } }
        obj_->emplace_back(key, std::move(v));
    }

private:
    Type type_;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;

    void dump_impl(std::string& out, int indent, int depth) const;
};

// Throws std::runtime_error with a descriptive message on malformed input.
Value parse(const std::string& text);
Value parse_file(const std::string& path);

} // namespace ddrtiming::json
