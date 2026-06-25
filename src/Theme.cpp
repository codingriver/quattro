#include "Theme.h"

#include "Utilities.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace {
float ByteToFloat(int value) {
    return static_cast<float>(std::max(0, std::min(255, value))) / 255.0f;
}

bool ParseNumberList(const std::wstring& text, std::vector<int>& out) {
    out.clear();
    std::wstring current;
    for (wchar_t ch : text) {
        if ((ch >= L'0' && ch <= L'9') || ch == L'-') {
            current += ch;
        } else if (!current.empty()) {
            auto value = ParseInt(current);
            if (!value) {
                return false;
            }
            out.push_back(*value);
            current.clear();
        }
    }
    if (!current.empty()) {
        auto value = ParseInt(current);
        if (!value) {
            return false;
        }
        out.push_back(*value);
    }
    return true;
}

int HexDigit(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return 10 + ch - L'a';
    if (ch >= L'A' && ch <= L'F') return 10 + ch - L'A';
    return -1;
}

int HexByte(const std::wstring& value, std::size_t index) {
    int hi = HexDigit(value[index]);
    int lo = HexDigit(value[index + 1]);
    if (hi < 0 || lo < 0) {
        return 0;
    }
    return hi * 16 + lo;
}

std::wstring AttributeValue(const std::wstring& tag, const std::wstring& name) {
    const std::wstring pattern = name + L"=\"";
    std::size_t begin = tag.find(pattern);
    if (begin == std::wstring::npos) {
        return {};
    }
    begin += pattern.size();
    const std::size_t end = tag.find(L'"', begin);
    if (end == std::wstring::npos) {
        return {};
    }
    return tag.substr(begin, end - begin);
}
}

Theme Theme::Load(const std::filesystem::path& themeDirectory, const std::wstring& themeName) {
    Theme theme;
    theme.SetDefaults();

    std::filesystem::path path = themeDirectory / (themeName + L".xml");
    if (!FileExists(path)) {
        path = themeDirectory / L"gray.xml";
    }

    const std::wstring xml = LoadUtf8File(path);
    if (!xml.empty()) {
        theme.ParseXml(xml);
        for (const auto& [name, raw] : theme.rawValues_) {
            theme.colors_[name] = theme.ResolveValue(raw);
        }
    }
    return theme;
}

Color Theme::get(const std::wstring& name) const {
    auto found = colors_.find(name);
    if (found != colors_.end()) {
        return found->second;
    }
    auto defaultColor = colors_.find(L"textdefaultcolor");
    if (defaultColor != colors_.end()) {
        return defaultColor->second;
    }
    return Color{0.2f, 0.2f, 0.2f, 1.0f};
}

void Theme::SetDefaults() {
    Put(L"black", Color{0, 0, 0, 1});
    Put(L"white", Color{1, 1, 1, 1});
    Put(L"blue", Color{0.15f, 0.39f, 0.92f, 1});
    Put(L"light_gray", Color{0.42f, 0.45f, 0.50f, 1});
    Put(L"textdefaultcolor", Color{0.12f, 0.14f, 0.16f, 1});
    Put(L"main_bd", Color{0.77f, 0.79f, 0.83f, 1});
    Put(L"main_bk", Color{1.0f, 1.0f, 1.0f, 1});
    Put(L"main_title", Color{0.15f, 0.39f, 0.92f, 1});
    Put(L"main_line", Color{0.85f, 0.87f, 0.89f, 1});
    Put(L"main_client", Color{0.96f, 0.97f, 0.98f, 1});
    Put(L"title_subtext", Color{0.42f, 0.45f, 0.50f, 1});
    Put(L"panel_bk", Color{0.98f, 0.99f, 1.0f, 1});
    Put(L"panel_alt", Color{0.94f, 0.96f, 0.98f, 1});
    Put(L"shadow_soft", Color{0.05f, 0.09f, 0.16f, 0.35f});
    Put(L"major_group_item_bk_hot", Color{0.91f, 0.94f, 1.0f, 1});
    Put(L"major_group_item_bk_sel", Color{1.0f, 1.0f, 1.0f, 1});
    Put(L"major_group_item_text_nml", Color{0.12f, 0.14f, 0.16f, 1});
    Put(L"major_group_item_text_sel", Color{0.15f, 0.39f, 0.92f, 1});
    Put(L"minor_group_item_bk_hot", Color{0.93f, 0.95f, 0.98f, 1});
    Put(L"minor_group_item_bk_sel", Color{0.89f, 0.93f, 1.0f, 1});
    Put(L"minor_group_item_text_nml", Color{0.12f, 0.14f, 0.16f, 1});
    Put(L"minor_group_item_text_sel", Color{0.12f, 0.14f, 0.16f, 1});
    Put(L"link_item_bk", Color{1.0f, 1.0f, 1.0f, 0.80f});
    Put(L"link_item_bk_hot", Color{1.0f, 1.0f, 1.0f, 1});
    Put(L"link_item_bk_sel", Color{0.90f, 0.94f, 1.0f, 1});
    Put(L"link_item_bd_hot", Color{0.77f, 0.82f, 0.90f, 1});
    Put(L"link_item_bd", Color{0.86f, 0.89f, 0.93f, 1});
    Put(L"link_item_text_nml", Color{0.12f, 0.14f, 0.16f, 1});
    Put(L"link_item_detail", Color{0.42f, 0.45f, 0.50f, 1});
    Put(L"menu_bk", Color{1.0f, 1.0f, 1.0f, 1});
    Put(L"menu_bd", Color{0.78f, 0.80f, 0.84f, 1});
    Put(L"menu_item_bk_hot", Color{0.92f, 0.95f, 1.0f, 1});
    Put(L"menu_item_bk_sel", Color{0.92f, 0.95f, 1.0f, 1});
    Put(L"menu_text", Color{0.12f, 0.14f, 0.16f, 1});
    Put(L"menu_text_disabled", Color{0.56f, 0.60f, 0.66f, 1});
    Put(L"scroll_track", Color{0.86f, 0.89f, 0.93f, 0.55f});
    Put(L"scroll_thumb", Color{0.58f, 0.65f, 0.75f, 0.85f});
    Put(L"empty_text", Color{0.38f, 0.43f, 0.50f, 1});
}

void Theme::ParseXml(const std::wstring& xml) {
    std::size_t pos = 0;
    while ((pos = xml.find(L"<Color", pos)) != std::wstring::npos) {
        const std::size_t end = xml.find(L">", pos);
        if (end == std::wstring::npos) {
            break;
        }
        const std::wstring tag = xml.substr(pos, end - pos + 1);
        const std::wstring name = AttributeValue(tag, L"name");
        const std::wstring value = AttributeValue(tag, L"value");
        if (!name.empty() && !value.empty()) {
            rawValues_[name] = value;
        }
        pos = end + 1;
    }
}

Color Theme::ResolveValue(const std::wstring& value, int depth) const {
    if (depth > 8) {
        return get(L"textdefaultcolor");
    }

    const std::wstring trimmed = Trim(value);
    const std::wstring lower = ToLower(trimmed);
    if (trimmed.size() == 9 && trimmed[0] == L'#') {
        return Color{
            ByteToFloat(HexByte(trimmed, 3)),
            ByteToFloat(HexByte(trimmed, 5)),
            ByteToFloat(HexByte(trimmed, 7)),
            ByteToFloat(HexByte(trimmed, 1)),
        };
    }

    if (lower.rfind(L"rgb(", 0) == 0) {
        std::vector<int> numbers;
        if (ParseNumberList(lower, numbers) && numbers.size() >= 3) {
            return Color{ByteToFloat(numbers[0]), ByteToFloat(numbers[1]), ByteToFloat(numbers[2]), 1.0f};
        }
    }

    if (lower.rfind(L"argb(", 0) == 0) {
        std::vector<int> numbers;
        if (ParseNumberList(lower, numbers) && numbers.size() >= 4) {
            return Color{ByteToFloat(numbers[1]), ByteToFloat(numbers[2]), ByteToFloat(numbers[3]), ByteToFloat(numbers[0])};
        }
    }

    if (!trimmed.empty() && trimmed[0] == L'@') {
        const std::wstring ref = trimmed.substr(1);
        auto color = colors_.find(ref);
        if (color != colors_.end()) {
            return color->second;
        }
        auto raw = rawValues_.find(ref);
        if (raw != rawValues_.end()) {
            return ResolveValue(raw->second, depth + 1);
        }
    }

    return get(L"textdefaultcolor");
}

void Theme::Put(const std::wstring& name, Color color) {
    colors_[name] = color;
}
