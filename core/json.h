// core/json.h — 最小化 JSON 解析/序列化（header-only）
// 仅用于 whitelist.json 和 settings.json 的读写
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace json {

// JSON 值类型
class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<Value> arr_val;
    std::map<std::string, Value> obj_val;

    Value() = default;
    explicit Value(bool b) : type(Type::Bool), bool_val(b) {}
    explicit Value(double n) : type(Type::Number), num_val(n) {}
    explicit Value(int n) : type(Type::Number), num_val(static_cast<double>(n)) {}
    explicit Value(const std::string& s) : type(Type::String), str_val(s) {}
    explicit Value(const char* s) : type(Type::String), str_val(s ? s : "") {}
    explicit Value(std::vector<Value> a) : type(Type::Array), arr_val(std::move(a)) {}
    explicit Value(std::map<std::string, Value> o) : type(Type::Object), obj_val(std::move(o)) {}

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    bool as_bool() const { return bool_val; }
    double as_number() const { return num_val; }
    int as_int() const { return static_cast<int>(num_val); }
    const std::string& as_string() const { return str_val; }
    const std::vector<Value>& as_array() const { return arr_val; }
    const std::map<std::string, Value>& as_object() const { return obj_val; }

    // 在 Object 中查找 key，找不到返回 nullptr
    const Value* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        auto it = obj_val.find(key);
        if (it == obj_val.end()) return nullptr;
        return &it->second;
    }
};

// ---- 解析器 ----

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text), pos_(0) {}

    Value parse() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        return v;
    }

private:
    std::string text_;
    size_t pos_;

    void skip_ws() {
        while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' ||
            text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
    }

    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    char get() {
        return pos_ < text_.size() ? text_[pos_++] : '\0';
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return Value(parse_string());
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        return parse_number();
    }

    Value parse_object() {
        std::map<std::string, Value> obj;
        get(); // consume '{'
        skip_ws();
        if (peek() == '}') { get(); return Value(std::move(obj)); }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (get() != ':') throw std::runtime_error("JSON: expected ':'");
            obj[key] = parse_value();
            skip_ws();
            char c = get();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("JSON: expected ',' or '}'");
        }
        return Value(std::move(obj));
    }

    Value parse_array() {
        std::vector<Value> arr;
        get(); // consume '['
        skip_ws();
        if (peek() == ']') { get(); return Value(std::move(arr)); }
        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            char c = get();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("JSON: expected ',' or ']'");
        }
        return Value(std::move(arr));
    }

    std::string parse_string() {
        if (get() != '"') throw std::runtime_error("JSON: expected '\"'");
        std::string s;
        while (true) {
            char c = get();
            if (c == '\0') throw std::runtime_error("JSON: unterminated string");
            if (c == '"') break;
            if (c == '\\') {
                c = get();
                switch (c) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'u': {
                        int code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char hex = get();
                            code <<= 4;
                            if (hex >= '0' && hex <= '9') code |= hex - '0';
                            else if (hex >= 'a' && hex <= 'f') code |= hex - 'a' + 10;
                            else if (hex >= 'A' && hex <= 'F') code |= hex - 'A' + 10;
                            else throw std::runtime_error("JSON: invalid unicode escape");
                        }
                        if (code < 0x80) {
                            s += static_cast<char>(code);
                        } else if (code < 0x800) {
                            s += static_cast<char>(0xC0 | (code >> 6));
                            s += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            s += static_cast<char>(0xE0 | (code >> 12));
                            s += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            s += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: throw std::runtime_error("JSON: invalid escape");
                }
            } else {
                s += c;
            }
        }
        return s;
    }

    Value parse_number() {
        size_t start = pos_;
        if (peek() == '-') get();
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' ||
                c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++pos_;
            } else {
                break;
            }
        }
        std::string num_str = text_.substr(start, pos_ - start);
        if (num_str.empty()) throw std::runtime_error("JSON: invalid number");
        try {
            return Value(std::stod(num_str));
        } catch (...) {
            throw std::runtime_error("JSON: invalid number");
        }
    }

    Value parse_bool() {
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return Value(true);
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return Value(false);
        }
        throw std::runtime_error("JSON: invalid boolean");
    }

    Value parse_null() {
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return Value();
        }
        throw std::runtime_error("JSON: invalid null");
    }
};

// ---- 顶层解析接口 ----

inline Value parse(const std::string& text) {
    Parser p(text);
    return p.parse();
}

// ---- 序列化 ----

inline std::string escape_string(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    result += buf;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

inline std::string serialize(const Value& v) {
    std::ostringstream ss;
    switch (v.type) {
        case Value::Type::Null:
            ss << "null";
            break;
        case Value::Type::Bool:
            ss << (v.bool_val ? "true" : "false");
            break;
        case Value::Type::Number: {
            double d = v.num_val;
            double intpart;
            if (std::modf(d, &intpart) == 0.0) {
                ss << static_cast<long long>(d);
            } else {
                ss << d;
            }
            break;
        }
        case Value::Type::String:
            ss << '"' << escape_string(v.str_val) << '"';
            break;
        case Value::Type::Array: {
            ss << '[';
            for (size_t i = 0; i < v.arr_val.size(); ++i) {
                if (i > 0) ss << ',';
                ss << serialize(v.arr_val[i]);
            }
            ss << ']';
            break;
        }
        case Value::Type::Object: {
            ss << '{';
            bool first = true;
            for (const auto& [k, val] : v.obj_val) {
                if (!first) ss << ',';
                first = false;
                ss << '"' << escape_string(k) << "\":" << serialize(val);
            }
            ss << '}';
            break;
        }
    }
    return ss.str();
}

} // namespace json
