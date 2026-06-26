#pragma once

#include <map>
#include <string>
#include <vector>

struct JsonValue {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool boolValue = false;
    double numberValue = 0.0;
    std::wstring stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::wstring, JsonValue> objectValue;

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isBool() const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }

    const JsonValue* get(const std::wstring& key) const;
    std::wstring stringOr(const std::wstring& fallback = L"") const;
    bool boolOr(bool fallback = false) const;
    int intOr(int fallback = 0) const;
};

bool ParseJson(const std::wstring& text, JsonValue& value, std::wstring& error);
