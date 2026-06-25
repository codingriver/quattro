#pragma once

#include <d2d1.h>

#include <filesystem>
#include <string>
#include <unordered_map>

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    D2D1_COLOR_F d2d() const {
        return D2D1::ColorF(r, g, b, a);
    }
};

class Theme {
public:
    static Theme Load(const std::filesystem::path& themeDirectory, const std::wstring& themeName);

    Color get(const std::wstring& name) const;

private:
    void SetDefaults();
    void ParseXml(const std::wstring& xml);
    Color ResolveValue(const std::wstring& value, int depth = 0) const;
    void Put(const std::wstring& name, Color color);

    std::unordered_map<std::wstring, Color> colors_;
    std::unordered_map<std::wstring, std::wstring> rawValues_;
};
