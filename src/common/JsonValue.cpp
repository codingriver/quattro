#include "JsonValue.h"

#include <cwctype>
#include <string>

namespace {
class Parser {
public:
    explicit Parser(const std::wstring& text) : text_(text) {}

    bool parse(JsonValue& value, std::wstring& error) {
        skipWhitespace();
        if (!parseValue(value)) {
            error = error_.empty() ? L"JSON 解析失败。" : error_;
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            error = L"JSON 末尾存在多余内容。";
            return false;
        }
        return true;
    }

private:
    void skipWhitespace() {
        while (pos_ < text_.size() && std::iswspace(text_[pos_])) {
            ++pos_;
        }
    }

    bool consume(wchar_t ch) {
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ch) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool parseValue(JsonValue& value) {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            error_ = L"JSON 值缺失。";
            return false;
        }
        const wchar_t ch = text_[pos_];
        if (ch == L'{') {
            return parseObject(value);
        }
        if (ch == L'[') {
            return parseArray(value);
        }
        if (ch == L'"') {
            value.type = JsonValue::Type::String;
            return parseString(value.stringValue);
        }
        if (ch == L't' && matchLiteral(L"true")) {
            value.type = JsonValue::Type::Bool;
            value.boolValue = true;
            return true;
        }
        if (ch == L'f' && matchLiteral(L"false")) {
            value.type = JsonValue::Type::Bool;
            value.boolValue = false;
            return true;
        }
        if (ch == L'n' && matchLiteral(L"null")) {
            value.type = JsonValue::Type::Null;
            return true;
        }
        if (ch == L'-' || (ch >= L'0' && ch <= L'9')) {
            return parseNumber(value);
        }
        error_ = L"JSON 值类型无效。";
        return false;
    }

    bool matchLiteral(const wchar_t* literal) {
        std::size_t len = 0;
        while (literal[len] != L'\0') {
            ++len;
        }
        if (text_.compare(pos_, len, literal) != 0) {
            return false;
        }
        pos_ += len;
        return true;
    }

    bool parseObject(JsonValue& value) {
        if (!consume(L'{')) {
            return false;
        }
        value.type = JsonValue::Type::Object;
        value.objectValue.clear();
        skipWhitespace();
        if (consume(L'}')) {
            return true;
        }
        while (pos_ < text_.size()) {
            std::wstring key;
            if (!parseString(key)) {
                return false;
            }
            if (!consume(L':')) {
                error_ = L"JSON 对象缺少冒号。";
                return false;
            }
            JsonValue child;
            if (!parseValue(child)) {
                return false;
            }
            value.objectValue.emplace(std::move(key), std::move(child));
            if (consume(L'}')) {
                return true;
            }
            if (!consume(L',')) {
                error_ = L"JSON 对象缺少逗号。";
                return false;
            }
        }
        error_ = L"JSON 对象未闭合。";
        return false;
    }

    bool parseArray(JsonValue& value) {
        if (!consume(L'[')) {
            return false;
        }
        value.type = JsonValue::Type::Array;
        value.arrayValue.clear();
        skipWhitespace();
        if (consume(L']')) {
            return true;
        }
        while (pos_ < text_.size()) {
            JsonValue child;
            if (!parseValue(child)) {
                return false;
            }
            value.arrayValue.push_back(std::move(child));
            if (consume(L']')) {
                return true;
            }
            if (!consume(L',')) {
                error_ = L"JSON 数组缺少逗号。";
                return false;
            }
        }
        error_ = L"JSON 数组未闭合。";
        return false;
    }

    bool parseString(std::wstring& value) {
        skipWhitespace();
        if (pos_ >= text_.size() || text_[pos_] != L'"') {
            error_ = L"JSON 字符串缺少引号。";
            return false;
        }
        ++pos_;
        value.clear();
        while (pos_ < text_.size()) {
            wchar_t ch = text_[pos_++];
            if (ch == L'"') {
                return true;
            }
            if (ch != L'\\') {
                value.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size()) {
                error_ = L"JSON 字符串转义不完整。";
                return false;
            }
            wchar_t esc = text_[pos_++];
            switch (esc) {
            case L'"': value.push_back(L'"'); break;
            case L'\\': value.push_back(L'\\'); break;
            case L'/': value.push_back(L'/'); break;
            case L'b': value.push_back(L'\b'); break;
            case L'f': value.push_back(L'\f'); break;
            case L'n': value.push_back(L'\n'); break;
            case L'r': value.push_back(L'\r'); break;
            case L't': value.push_back(L'\t'); break;
            case L'u':
                if (!parseUnicodeEscape(value)) {
                    return false;
                }
                break;
            default:
                error_ = L"JSON 字符串转义无效。";
                return false;
            }
        }
        error_ = L"JSON 字符串未闭合。";
        return false;
    }

    bool parseUnicodeEscape(std::wstring& value) {
        if (pos_ + 4 > text_.size()) {
            error_ = L"JSON unicode 转义不完整。";
            return false;
        }
        int code = 0;
        for (int i = 0; i < 4; ++i) {
            const wchar_t ch = text_[pos_++];
            int digit = -1;
            if (ch >= L'0' && ch <= L'9') digit = ch - L'0';
            if (ch >= L'a' && ch <= L'f') digit = 10 + ch - L'a';
            if (ch >= L'A' && ch <= L'F') digit = 10 + ch - L'A';
            if (digit < 0) {
                error_ = L"JSON unicode 转义无效。";
                return false;
            }
            code = code * 16 + digit;
        }
        value.push_back(static_cast<wchar_t>(code));
        return true;
    }

    bool parseNumber(JsonValue& value) {
        const std::size_t begin = pos_;
        if (text_[pos_] == L'-') {
            ++pos_;
        }
        while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9') {
            ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == L'.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9') {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == L'e' || text_[pos_] == L'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == L'+' || text_[pos_] == L'-')) {
                ++pos_;
            }
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9') {
                ++pos_;
            }
        }
        try {
            value.numberValue = std::stod(text_.substr(begin, pos_ - begin));
        } catch (...) {
            error_ = L"JSON 数字无效。";
            return false;
        }
        value.type = JsonValue::Type::Number;
        return true;
    }

    const std::wstring& text_;
    std::size_t pos_ = 0;
    std::wstring error_;
};
}

const JsonValue* JsonValue::get(const std::wstring& key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    const auto it = objectValue.find(key);
    return it == objectValue.end() ? nullptr : &it->second;
}

std::wstring JsonValue::stringOr(const std::wstring& fallback) const {
    return type == Type::String ? stringValue : fallback;
}

bool JsonValue::boolOr(bool fallback) const {
    return type == Type::Bool ? boolValue : fallback;
}

int JsonValue::intOr(int fallback) const {
    return type == Type::Number ? static_cast<int>(numberValue) : fallback;
}

bool ParseJson(const std::wstring& text, JsonValue& value, std::wstring& error) {
    Parser parser(text);
    return parser.parse(value, error);
}
