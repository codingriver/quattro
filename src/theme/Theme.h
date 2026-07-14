#pragma once

#include <d2d1.h>
#include <windows.h>

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
    static Theme Load(
        const std::filesystem::path& themeDirectory,
        const std::wstring& themeName,
        HINSTANCE fallbackModule,
        int fallbackResourceId);

    Color color(const std::wstring& component, const std::wstring& state, const std::wstring& role) const;
    float metric(const std::wstring& component, const std::wstring& name, float fallback) const;

private:
    void SetDefaults();
    void ParseXml(const std::wstring& xml);
    Color ResolveColorValue(const std::wstring& value, int depth = 0) const;
    void PutPalette(const std::wstring& name, Color color);
    void PutState(const std::wstring& component, const std::wstring& state, const std::wstring& role, Color color);
    void PutMetric(const std::wstring& component, const std::wstring& name, float value);

    std::unordered_map<std::wstring, Color> palette_;
    std::unordered_map<std::wstring, std::wstring> rawPalette_;
    std::unordered_map<std::wstring, Color> stateColors_;
    std::unordered_map<std::wstring, std::wstring> rawStateColors_;
    std::unordered_map<std::wstring, float> metrics_;
};
