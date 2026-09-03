#include "json.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ddrtiming::json {

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text), pos_(0), len_(text.size()) {}

    Value parse_document() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != len_) fail("trailing content after JSON document");
        return v;
    }

private:
    const std::string& s_;
    size_t pos_;
    size_t len_;

    [[noreturn]] void fail(const std::string& msg) {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < pos_ && i < len_; ++i) {
            if (s_[i] == '\n') { ++line; col = 1; } else { ++col; }
        }
        std::ostringstream os;
        os << "JSON parse error at line " << line << ", col " << col << ": " << msg;
        throw std::runtime_error(os.str());
    }

    char peek() { return pos_ < len_ ? s_[pos_] : '\0'; }
    char advance() { return pos_ < len_ ? s_[pos_++] : '\0'; }

    void skip_ws() {
        while (pos_ < len_) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos_; continue; }
            if (c == '/' && pos_ + 1 < len_ && s_[pos_ + 1] == '/') { // tolerate // comments
                while (pos_ < len_ && s_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }

    void expect(char c) {
        if (peek() != c) fail(std::string("expected '") + c + "'");
        ++pos_;
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return Value(parse_string());
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
        fail("unexpected character");
    }

    Value parse_object() {
        expect('{');
        Object obj;
        skip_ws();
        if (peek() == '}') { ++pos_; return Value(std::move(obj)); }
        while (true) {
            skip_ws();
            if (peek() != '"') fail("expected string key");
            std::string key = parse_string();
            skip_ws();
            expect(':');
            Value val = parse_value();
            obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            char c = advance();
            if (c == ',') continue;
            if (c == '}') break;
            fail("expected ',' or '}' in object");
        }
        return Value(std::move(obj));
    }

    Value parse_array() {
        expect('[');
        Array arr;
        skip_ws();
        if (peek() == ']') { ++pos_; return Value(std::move(arr)); }
        while (true) {
            Value val = parse_value();
            arr.push_back(std::move(val));
            skip_ws();
            char c = advance();
            if (c == ',') continue;
            if (c == ']') break;
            fail("expected ',' or ']' in array");
        }
        return Value(std::move(arr));
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= len_) fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') break;
            if (c == '\\') {
                if (pos_ >= len_) fail("unterminated escape");
                char e = s_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > len_) fail("bad \\u escape");
                        // Minimal handling: only BMP code points, emitted as raw byte if ASCII, '?' otherwise.
                        std::string hex = s_.substr(pos_, 4);
                        pos_ += 4;
                        unsigned int code = static_cast<unsigned int>(std::strtoul(hex.c_str(), nullptr, 16));
                        if (code < 0x80) out.push_back(static_cast<char>(code));
                        else out.push_back('?');
                        break;
                    }
                    default: fail("invalid escape character");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    Value parse_bool() {
        if (s_.compare(pos_, 4, "true") == 0) { pos_ += 4; return Value(true); }
        if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; return Value(false); }
        fail("invalid literal");
    }

    Value parse_null() {
        if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; return Value(nullptr); }
        fail("invalid literal");
    }

    Value parse_number() {
        size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        if (peek() == '.') { ++pos_; while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_; }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        std::string numstr = s_.substr(start, pos_ - start);
        if (numstr.empty() || numstr == "-") fail("invalid number");
        return Value(std::strtod(numstr.c_str(), nullptr));
    }
};

void escape_into(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void format_number(std::string& out, double d) {
    if (d == static_cast<double>(static_cast<int64_t>(d)) &&
        d < 1e15 && d > -1e15) {
        out += std::to_string(static_cast<int64_t>(d));
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", d);
        out += buf;
    }
}

} // namespace

Value parse(const std::string& text) {
    Parser p(text);
    return p.parse_document();
}

Value parse_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open JSON file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

void Value::dump_impl(std::string& out, int indent, int depth) const {
    auto pad = [&](int d) { if (indent > 0) out.append(static_cast<size_t>(indent) * d, ' '); };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: format_number(out, num_); break;
        case Type::String: escape_into(out, str_); break;
        case Type::Array: {
            if (!arr_ || arr_->empty()) { out += "[]"; break; }
            out += "[";
            if (indent > 0) out += "\n";
            for (size_t i = 0; i < arr_->size(); ++i) {
                pad(depth + 1);
                (*arr_)[i].dump_impl(out, indent, depth + 1);
                if (i + 1 < arr_->size()) out += ",";
                if (indent > 0) out += "\n";
            }
            pad(depth);
            out += "]";
            break;
        }
        case Type::Object: {
            if (!obj_ || obj_->empty()) { out += "{}"; break; }
            out += "{";
            if (indent > 0) out += "\n";
            for (size_t i = 0; i < obj_->size(); ++i) {
                pad(depth + 1);
                escape_into(out, (*obj_)[i].first);
                out += indent > 0 ? ": " : ":";
                (*obj_)[i].second.dump_impl(out, indent, depth + 1);
                if (i + 1 < obj_->size()) out += ",";
                if (indent > 0) out += "\n";
            }
            pad(depth);
            out += "}";
            break;
        }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dump_impl(out, indent, 0);
    return out;
}

} // namespace ddrtiming::json
