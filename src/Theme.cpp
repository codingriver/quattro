#include "Theme.h"

#include "Utilities.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <optional>
#include <vector>
#include <windows.h>

namespace {
constexpr const wchar_t* kGlobalComponent = L"global";
constexpr const wchar_t* kNormalState = L"normal";

bool IsAllowedComponent(const std::wstring& component) {
    // Theme rules allow shared categories only, not page/control-instance overrides.
    static constexpr const wchar_t* kAllowedComponents[] = {
        L"global",
        L"window",
        L"title",
        L"titleButton",
        L"titleCloseButton",
        L"majorNav",
        L"majorNavItem",
        L"minorNav",
        L"minorNavItem",
        L"content",
        L"text",
        L"label",
        L"panel",
        L"field",
        L"linkItem",
        L"iconFallback",
        L"scrollbar",
        L"menu",
        L"menuItem",
        L"dialog",
        L"edit",
        L"iconButton",
        L"miniButton",
        L"comboBox",
        L"tabButton",
        L"button",
        L"checkbox",
        L"toggle",
        L"radio",
        L"list",
        L"listItem",
        L"slider",
        L"tooltip",
        L"separator",
    };
    for (const wchar_t* allowed : kAllowedComponents) {
        if (component == allowed) {
            return true;
        }
    }
    return false;
}

void ReportUnsupportedComponent(const std::wstring& component) {
    OutputDebugStringW((L"Quattro theme warning: unsupported component ignored: " + component + L"\n").c_str());
}

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

std::unordered_map<std::wstring, std::wstring> Attributes(const std::wstring& tag) {
    std::unordered_map<std::wstring, std::wstring> values;
    std::size_t pos = tag.find(L' ');
    while (pos != std::wstring::npos && pos < tag.size()) {
        while (pos < tag.size() && std::iswspace(tag[pos])) {
            ++pos;
        }
        if (pos >= tag.size() || tag[pos] == L'/' || tag[pos] == L'>') {
            break;
        }

        const std::size_t nameBegin = pos;
        while (pos < tag.size() && tag[pos] != L'=' && !std::iswspace(tag[pos]) && tag[pos] != L'>' && tag[pos] != L'/') {
            ++pos;
        }
        const std::wstring name = tag.substr(nameBegin, pos - nameBegin);
        while (pos < tag.size() && std::iswspace(tag[pos])) {
            ++pos;
        }
        if (pos >= tag.size() || tag[pos] != L'=') {
            continue;
        }
        ++pos;
        while (pos < tag.size() && std::iswspace(tag[pos])) {
            ++pos;
        }
        if (pos >= tag.size() || tag[pos] != L'"') {
            continue;
        }
        ++pos;
        const std::size_t valueBegin = pos;
        const std::size_t valueEnd = tag.find(L'"', pos);
        if (valueEnd == std::wstring::npos) {
            break;
        }
        if (!name.empty()) {
            values[name] = tag.substr(valueBegin, valueEnd - valueBegin);
        }
        pos = valueEnd + 1;
    }
    return values;
}

std::optional<float> ParseFloatValue(const std::wstring& text) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    const float value = std::wcstof(trimmed.c_str(), &end);
    if (!end || *end != L'\0') {
        return std::nullopt;
    }
    return value;
}

std::wstring Key(const std::wstring& a, const std::wstring& b) {
    return a + L"\x1f" + b;
}

std::wstring Key(const std::wstring& a, const std::wstring& b, const std::wstring& c) {
    return a + L"\x1f" + b + L"\x1f" + c;
}

std::wstring FindBlock(const std::wstring& xml, const std::wstring& name) {
    const std::wstring open = L"<" + name;
    const std::wstring close = L"</" + name + L">";
    const std::size_t begin = xml.find(open);
    if (begin == std::wstring::npos) {
        return {};
    }
    const std::size_t contentBegin = xml.find(L">", begin);
    if (contentBegin == std::wstring::npos) {
        return {};
    }
    const std::size_t end = xml.find(close, contentBegin + 1);
    if (end == std::wstring::npos) {
        return {};
    }
    return xml.substr(contentBegin + 1, end - contentBegin - 1);
}
}

Theme Theme::Load(const std::filesystem::path& themeDirectory, const std::wstring& themeName) {
    Theme theme;
    theme.SetDefaults();

    std::filesystem::path path = themeDirectory / (themeName + L".xml");
    if (!FileExists(path)) {
        path = themeDirectory / L"default.xml";
    }

    const std::wstring xml = LoadUtf8File(path);
    if (!xml.empty()) {
        theme.ParseXml(xml);
        for (const auto& [name, raw] : theme.rawPalette_) {
            theme.palette_[name] = theme.ResolveColorValue(raw);
        }
        for (const auto& [name, raw] : theme.rawStateColors_) {
            theme.stateColors_[name] = theme.ResolveColorValue(raw);
        }
    }
    return theme;
}

Color Theme::color(const std::wstring& component, const std::wstring& state, const std::wstring& role) const {
    auto found = stateColors_.find(Key(component, state, role));
    if (found != stateColors_.end()) {
        return found->second;
    }
    if (state != kNormalState) {
        found = stateColors_.find(Key(component, kNormalState, role));
        if (found != stateColors_.end()) {
            return found->second;
        }
    }
    found = stateColors_.find(Key(kGlobalComponent, kNormalState, role));
    if (found != stateColors_.end()) {
        return found->second;
    }
    auto palette = palette_.find(role);
    if (palette != palette_.end()) {
        return palette->second;
    }
    palette = palette_.find(L"text");
    if (palette != palette_.end()) {
        return palette->second;
    }
    return Color{0.2f, 0.2f, 0.2f, 1.0f};
}

float Theme::metric(const std::wstring& component, const std::wstring& name, float fallback) const {
    auto found = metrics_.find(Key(component, name));
    if (found != metrics_.end()) {
        return found->second;
    }
    found = metrics_.find(Key(kGlobalComponent, name));
    return found != metrics_.end() ? found->second : fallback;
}

void Theme::SetDefaults() {
    PutPalette(L"black", Color{0, 0, 0, 1});
    PutPalette(L"white", Color{1, 1, 1, 1});
    PutPalette(L"text", Color{0.12f, 0.14f, 0.16f, 1});
    PutPalette(L"mutedText", Color{0.42f, 0.45f, 0.50f, 1});
    PutPalette(L"accent", Color{0.15f, 0.39f, 0.92f, 1});
    PutPalette(L"accentSoft", Color{0.89f, 0.93f, 1.0f, 1});
    PutPalette(L"focus", Color{0.15f, 0.39f, 0.92f, 1});
    PutPalette(L"surface", Color{1, 1, 1, 1});
    PutPalette(L"surfaceRaised", Color{0.98f, 0.99f, 1.0f, 1});
    PutPalette(L"background", Color{0.96f, 0.97f, 0.98f, 1});
    PutPalette(L"border", Color{0.77f, 0.79f, 0.83f, 1});
    PutPalette(L"line", Color{0.85f, 0.87f, 0.89f, 1});
    PutPalette(L"hover", Color{0.92f, 0.95f, 1, 1});
    PutPalette(L"pressed", Color{0.86f, 0.91f, 1.0f, 1});
    PutPalette(L"selected", Color{0.90f, 0.94f, 1, 1});
    PutPalette(L"dangerSoft", Color{0.92f, 0.22f, 0.22f, 0.16f});
    PutPalette(L"danger", Color{0.72f, 0.10f, 0.10f, 1});
    PutPalette(L"success", Color{0.04f, 0.50f, 0.32f, 1});
    PutPalette(L"successSoft", Color{0.87f, 0.97f, 0.92f, 1});
    PutPalette(L"warning", Color{0.74f, 0.40f, 0.03f, 1});
    PutPalette(L"warningSoft", Color{1.0f, 0.95f, 0.82f, 1});
    PutPalette(L"info", Color{0.10f, 0.42f, 0.76f, 1});
    PutPalette(L"infoSoft", Color{0.88f, 0.94f, 1.0f, 1});
    PutPalette(L"overlay", Color{0.05f, 0.07f, 0.10f, 0.42f});
    PutPalette(L"shadow", Color{0.05f, 0.07f, 0.10f, 0.20f});
    PutPalette(L"disabledText", Color{0.56f, 0.60f, 0.66f, 1});
    PutPalette(L"scrollTrack", Color{0.86f, 0.89f, 0.93f, 0.55f});
    PutPalette(L"scrollThumb", Color{0.58f, 0.65f, 0.75f, 0.85f});

    PutState(L"global", L"normal", L"text", palette_[L"text"]);
    PutState(L"global", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"global", L"success", L"text", palette_[L"success"]);
    PutState(L"global", L"warning", L"text", palette_[L"warning"]);
    PutState(L"global", L"danger", L"text", palette_[L"danger"]);
    PutState(L"global", L"info", L"text", palette_[L"info"]);
    PutState(L"window", L"normal", L"bg", palette_[L"background"]);
    PutState(L"window", L"normal", L"border", palette_[L"border"]);
    PutState(L"title", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"title", L"normal", L"text", palette_[L"accent"]);
    PutState(L"title", L"normal", L"subtext", palette_[L"mutedText"]);
    PutState(L"title", L"normal", L"line", palette_[L"line"]);
    PutState(L"titleButton", L"normal", L"icon", palette_[L"text"]);
    PutState(L"titleButton", L"hover", L"bg", palette_[L"hover"]);
    PutState(L"titleCloseButton", L"hover", L"bg", palette_[L"dangerSoft"]);
    PutState(L"titleCloseButton", L"hover", L"icon", palette_[L"danger"]);
    PutState(L"majorNav", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"majorNav", L"normal", L"line", palette_[L"line"]);
    PutState(L"majorNavItem", L"normal", L"text", palette_[L"text"]);
    PutState(L"majorNavItem", L"hover", L"bg", palette_[L"hover"]);
    PutState(L"majorNavItem", L"selected", L"bg", palette_[L"surface"]);
    PutState(L"majorNavItem", L"selected", L"text", palette_[L"accent"]);
    PutState(L"minorNav", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"minorNav", L"normal", L"line", palette_[L"line"]);
    PutState(L"minorNavItem", L"normal", L"text", palette_[L"text"]);
    PutState(L"minorNavItem", L"hover", L"bg", Color{0.93f, 0.95f, 0.98f, 1});
    PutState(L"minorNavItem", L"selected", L"bg", palette_[L"selected"]);
    PutState(L"minorNavItem", L"selected", L"text", palette_[L"text"]);
    PutState(L"minorNavItem", L"selected", L"accent", palette_[L"accent"]);
    PutState(L"content", L"normal", L"bg", palette_[L"background"]);
    PutState(L"content", L"empty", L"text", Color{0.38f, 0.43f, 0.50f, 1});
    PutState(L"text", L"normal", L"text", palette_[L"text"]);
    PutState(L"text", L"muted", L"text", palette_[L"mutedText"]);
    PutState(L"text", L"disabled", L"text", palette_[L"disabledText"]);
    PutState(L"text", L"accent", L"text", palette_[L"accent"]);
    PutState(L"label", L"normal", L"text", palette_[L"text"]);
    PutState(L"label", L"muted", L"text", palette_[L"mutedText"]);
    PutState(L"label", L"disabled", L"text", palette_[L"disabledText"]);
    PutState(L"panel", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"panel", L"normal", L"border", palette_[L"line"]);
    PutState(L"panel", L"raised", L"bg", palette_[L"surfaceRaised"]);
    PutState(L"panel", L"raised", L"border", palette_[L"border"]);
    PutState(L"field", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"field", L"normal", L"text", palette_[L"text"]);
    PutState(L"field", L"normal", L"border", palette_[L"border"]);
    PutState(L"field", L"readonly", L"bg", palette_[L"surfaceRaised"]);
    PutState(L"field", L"readonly", L"text", palette_[L"mutedText"]);
    PutState(L"field", L"readonly", L"border", palette_[L"line"]);
    PutState(L"linkItem", L"normal", L"bg", Color{1, 1, 1, 0.80f});
    PutState(L"linkItem", L"normal", L"text", palette_[L"text"]);
    PutState(L"linkItem", L"normal", L"subtext", palette_[L"mutedText"]);
    PutState(L"linkItem", L"hover", L"bg", palette_[L"surface"]);
    PutState(L"linkItem", L"pressed", L"bg", palette_[L"pressed"]);
    PutState(L"linkItem", L"selected", L"bg", palette_[L"selected"]);
    PutState(L"linkItem", L"selected", L"accent", palette_[L"accent"]);
    PutState(L"linkItem", L"disabled", L"text", palette_[L"disabledText"]);
    PutState(L"linkItem", L"dragTarget", L"bg", palette_[L"accentSoft"]);
    PutState(L"linkItem", L"dragTarget", L"border", palette_[L"accent"]);
    PutState(L"iconFallback", L"normal", L"bg", palette_[L"accent"]);
    PutState(L"iconFallback", L"normal", L"border", palette_[L"border"]);
    PutState(L"scrollbar", L"normal", L"track", palette_[L"scrollTrack"]);
    PutState(L"scrollbar", L"normal", L"thumb", palette_[L"scrollThumb"]);
    PutState(L"menu", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"menuItem", L"normal", L"text", palette_[L"text"]);
    PutState(L"menuItem", L"normal", L"icon", palette_[L"mutedText"]);
    PutState(L"menuItem", L"disabled", L"text", palette_[L"disabledText"]);
    PutState(L"menuItem", L"disabled", L"icon", palette_[L"disabledText"]);
    PutState(L"menuItem", L"hover", L"bg", palette_[L"hover"]);
    PutState(L"menuItem", L"hover", L"icon", palette_[L"mutedText"]);
    PutState(L"menuItem", L"checked", L"mark", palette_[L"accent"]);
    PutState(L"menuItem", L"checked", L"icon", palette_[L"accent"]);
    PutState(L"dialog", L"normal", L"bg", palette_[L"background"]);
    PutState(L"dialog", L"normal", L"text", palette_[L"text"]);
    PutState(L"iconButton", L"normal", L"bg", Color{1, 1, 1, 0});
    PutState(L"iconButton", L"normal", L"icon", palette_[L"text"]);
    PutState(L"iconButton", L"hover", L"bg", palette_[L"hover"]);
    PutState(L"iconButton", L"hover", L"icon", palette_[L"accent"]);
    PutState(L"iconButton", L"pressed", L"bg", palette_[L"pressed"]);
    PutState(L"iconButton", L"disabled", L"icon", palette_[L"disabledText"]);
    PutState(L"miniButton", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"miniButton", L"normal", L"border", palette_[L"border"]);
    PutState(L"miniButton", L"normal", L"icon", palette_[L"mutedText"]);
    PutState(L"miniButton", L"hover", L"bg", palette_[L"hover"]);
    PutState(L"miniButton", L"hover", L"border", palette_[L"accent"]);
    PutState(L"miniButton", L"hover", L"icon", palette_[L"accent"]);
    PutState(L"miniButton", L"pressed", L"bg", palette_[L"pressed"]);
    PutState(L"miniButton", L"pressed", L"border", palette_[L"accent"]);
    PutState(L"miniButton", L"pressed", L"icon", palette_[L"accent"]);
    PutState(L"miniButton", L"focused", L"border", palette_[L"focus"]);
    PutState(L"miniButton", L"disabled", L"bg", palette_[L"background"]);
    PutState(L"miniButton", L"disabled", L"border", palette_[L"line"]);
    PutState(L"miniButton", L"disabled", L"icon", palette_[L"disabledText"]);
    PutState(L"edit", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"edit", L"normal", L"text", palette_[L"text"]);
    PutState(L"edit", L"normal", L"border", palette_[L"border"]);
    PutState(L"edit", L"hover", L"bg", palette_[L"surface"]);
    PutState(L"edit", L"hover", L"text", palette_[L"text"]);
    PutState(L"edit", L"hover", L"border", palette_[L"accent"]);
    PutState(L"edit", L"focused", L"bg", palette_[L"surface"]);
    PutState(L"edit", L"focused", L"text", palette_[L"text"]);
    PutState(L"edit", L"focused", L"border", palette_[L"focus"]);
    PutState(L"edit", L"readonly", L"bg", palette_[L"surfaceRaised"]);
    PutState(L"edit", L"readonly", L"text", palette_[L"mutedText"]);
    PutState(L"edit", L"readonly", L"border", palette_[L"line"]);
    PutState(L"edit", L"disabled", L"bg", palette_[L"background"]);
    PutState(L"edit", L"disabled", L"text", palette_[L"disabledText"]);
    PutState(L"edit", L"disabled", L"border", palette_[L"line"]);
    PutState(L"edit", L"error", L"bg", palette_[L"dangerSoft"]);
    PutState(L"edit", L"error", L"text", palette_[L"text"]);
    PutState(L"edit", L"error", L"border", palette_[L"danger"]);
    PutState(L"comboBox", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"comboBox", L"normal", L"text", palette_[L"text"]);
    PutState(L"comboBox", L"normal", L"border", palette_[L"border"]);
    PutState(L"comboBox", L"normal", L"itemBg", palette_[L"surface"]);
    PutState(L"comboBox", L"normal", L"arrow", palette_[L"mutedText"]);
    PutState(L"comboBox", L"hover", L"bg", palette_[L"surface"]);
    PutState(L"comboBox", L"hover", L"text", palette_[L"text"]);
    PutState(L"comboBox", L"hover", L"border", palette_[L"accent"]);
    PutState(L"comboBox", L"hover", L"itemBg", palette_[L"hover"]);
    PutState(L"comboBox", L"hover", L"arrow", palette_[L"accent"]);
    PutState(L"comboBox", L"focused", L"bg", palette_[L"surface"]);
    PutState(L"comboBox", L"focused", L"text", palette_[L"text"]);
    PutState(L"comboBox", L"focused", L"border", palette_[L"focus"]);
    PutState(L"comboBox", L"focused", L"itemBg", palette_[L"surface"]);
    PutState(L"comboBox", L"focused", L"arrow", palette_[L"accent"]);
    PutState(L"comboBox", L"selected", L"bg", palette_[L"surface"]);
    PutState(L"comboBox", L"selected", L"text", palette_[L"text"]);
    PutState(L"comboBox", L"selected", L"border", palette_[L"accent"]);
    PutState(L"comboBox", L"selected", L"itemBg", palette_[L"selected"]);
    PutState(L"comboBox", L"selected", L"arrow", palette_[L"accent"]);
    PutState(L"comboBox", L"disabled", L"bg", palette_[L"background"]);
    PutState(L"comboBox", L"disabled", L"text", palette_[L"disabledText"]);
    PutState(L"comboBox", L"disabled", L"border", palette_[L"line"]);
    PutState(L"comboBox", L"disabled", L"itemBg", palette_[L"background"]);
    PutState(L"comboBox", L"disabled", L"arrow", palette_[L"disabledText"]);
    PutState(L"tabButton", L"normal", L"bg", palette_[L"surfaceRaised"]);
    PutState(L"tabButton", L"normal", L"text", palette_[L"text"]);
    PutState(L"tabButton", L"normal", L"border", Color{0.82f, 0.85f, 0.89f, 1});
    PutState(L"tabButton", L"normal", L"groupBg", palette_[L"surfaceRaised"]);
    PutState(L"tabButton", L"normal", L"groupBorder", palette_[L"border"]);
    PutState(L"tabButton", L"hover", L"bg", palette_[L"hover"]);
    PutState(L"tabButton", L"hover", L"text", palette_[L"text"]);
    PutState(L"tabButton", L"hover", L"border", palette_[L"border"]);
    PutState(L"tabButton", L"selected", L"bg", palette_[L"accentSoft"]);
    PutState(L"tabButton", L"selected", L"text", palette_[L"accent"]);
    PutState(L"tabButton", L"selected", L"border", palette_[L"accent"]);
    PutState(L"tabButton", L"selectedHover", L"bg", palette_[L"pressed"]);
    PutState(L"tabButton", L"selectedHover", L"text", palette_[L"accent"]);
    PutState(L"tabButton", L"selectedHover", L"border", palette_[L"accent"]);
    PutState(L"tabButton", L"focused", L"border", palette_[L"focus"]);
    PutState(L"tabButton", L"disabled", L"bg", palette_[L"background"]);
    PutState(L"tabButton", L"disabled", L"text", palette_[L"disabledText"]);
    PutState(L"tabButton", L"disabled", L"border", palette_[L"line"]);
    PutState(L"button", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"button", L"normal", L"text", palette_[L"text"]);
    PutState(L"button", L"normal", L"border", palette_[L"border"]);
    PutState(L"button", L"hover", L"bg", palette_[L"hover"]);
    PutState(L"button", L"hover", L"text", palette_[L"text"]);
    PutState(L"button", L"hover", L"border", palette_[L"accent"]);
    PutState(L"button", L"pressed", L"bg", palette_[L"pressed"]);
    PutState(L"button", L"pressed", L"text", palette_[L"text"]);
    PutState(L"button", L"pressed", L"border", palette_[L"accent"]);
    PutState(L"button", L"focused", L"border", palette_[L"focus"]);
    PutState(L"button", L"disabled", L"bg", palette_[L"background"]);
    PutState(L"button", L"disabled", L"text", palette_[L"disabledText"]);
    PutState(L"button", L"disabled", L"border", palette_[L"line"]);
    PutState(L"checkbox", L"normal", L"text", palette_[L"text"]);
    PutState(L"checkbox", L"normal", L"boxBg", palette_[L"surface"]);
    PutState(L"checkbox", L"normal", L"border", palette_[L"border"]);
    PutState(L"checkbox", L"hover", L"text", palette_[L"text"]);
    PutState(L"checkbox", L"hover", L"boxBg", palette_[L"hover"]);
    PutState(L"checkbox", L"hover", L"border", palette_[L"accent"]);
    PutState(L"checkbox", L"checked", L"text", palette_[L"text"]);
    PutState(L"checkbox", L"checked", L"boxBg", palette_[L"accent"]);
    PutState(L"checkbox", L"checked", L"border", palette_[L"accent"]);
    PutState(L"checkbox", L"checked", L"mark", palette_[L"white"]);
    PutState(L"checkbox", L"checkedHover", L"text", palette_[L"text"]);
    PutState(L"checkbox", L"checkedHover", L"boxBg", palette_[L"accent"]);
    PutState(L"checkbox", L"checkedHover", L"border", palette_[L"focus"]);
    PutState(L"checkbox", L"checkedHover", L"mark", palette_[L"white"]);
    PutState(L"checkbox", L"focused", L"border", palette_[L"focus"]);
    PutState(L"checkbox", L"disabled", L"text", palette_[L"disabledText"]);
    PutState(L"checkbox", L"disabled", L"boxBg", palette_[L"background"]);
    PutState(L"checkbox", L"disabled", L"border", palette_[L"line"]);
    PutState(L"toggle", L"normal", L"track", palette_[L"border"]);
    PutState(L"toggle", L"normal", L"thumb", palette_[L"surface"]);
    PutState(L"toggle", L"checked", L"track", palette_[L"accent"]);
    PutState(L"toggle", L"checked", L"thumb", palette_[L"surface"]);
    PutState(L"toggle", L"disabled", L"track", palette_[L"line"]);
    PutState(L"toggle", L"disabled", L"thumb", palette_[L"surfaceRaised"]);
    PutState(L"radio", L"normal", L"text", palette_[L"text"]);
    PutState(L"radio", L"normal", L"dot", palette_[L"surface"]);
    PutState(L"radio", L"normal", L"border", palette_[L"border"]);
    PutState(L"radio", L"checked", L"text", palette_[L"text"]);
    PutState(L"radio", L"checked", L"dot", palette_[L"accent"]);
    PutState(L"radio", L"checked", L"border", palette_[L"accent"]);
    PutState(L"list", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"list", L"normal", L"text", palette_[L"text"]);
    PutState(L"list", L"normal", L"border", palette_[L"border"]);
    PutState(L"list", L"readonly", L"bg", palette_[L"surfaceRaised"]);
    PutState(L"listItem", L"normal", L"bg", palette_[L"surface"]);
    PutState(L"listItem", L"normal", L"text", palette_[L"text"]);
    PutState(L"listItem", L"hover", L"bg", palette_[L"hover"]);
    PutState(L"listItem", L"selected", L"bg", palette_[L"selected"]);
    PutState(L"listItem", L"selected", L"text", palette_[L"text"]);
    PutState(L"slider", L"normal", L"track", palette_[L"line"]);
    PutState(L"slider", L"normal", L"fill", palette_[L"accent"]);
    PutState(L"slider", L"normal", L"thumb", palette_[L"surface"]);
    PutState(L"slider", L"normal", L"border", palette_[L"border"]);
    PutState(L"tooltip", L"normal", L"bg", Color{229.0f / 255.0f, 231.0f / 255.0f, 235.0f / 255.0f, 1.0f});
    PutState(L"tooltip", L"normal", L"text", Color{17.0f / 255.0f, 24.0f / 255.0f, 39.0f / 255.0f, 1.0f});
    PutState(L"tooltip", L"normal", L"border", Color{156.0f / 255.0f, 163.0f / 255.0f, 175.0f / 255.0f, 1.0f});
    PutState(L"separator", L"normal", L"line", palette_[L"line"]);

    PutMetric(L"titleButton", L"size", 26.0f);
    PutMetric(L"titleButton", L"gap", 2.0f);
    PutMetric(L"titleButton", L"rightInset", 4.0f);
    PutMetric(L"titleButton", L"topInset", 4.0f);
    PutMetric(L"titleButton", L"reserveInset", 16.0f);
    PutMetric(L"titleButton", L"iconStrokeWidth", 1.6f);
    PutMetric(L"titleButton", L"iconHalf", 5.0f);
    PutMetric(L"titleButton", L"menuHalfWidth", 6.0f);
    PutMetric(L"titleButton", L"menuLineGap", 5.0f);
    PutMetric(L"titleButton", L"searchRadius", 5.0f);
    PutMetric(L"titleButton", L"searchOffset", 2.0f);
    PutMetric(L"titleButton", L"searchHandle", 7.0f);
    PutMetric(L"titleButton", L"skinStrokeWidth", 1.4f);
    PutMetric(L"titleButton", L"skinLineStrokeWidth", 1.2f);
    PutMetric(L"title", L"height", 34.0f);
    PutMetric(L"title", L"iconSize", 20.0f);
    PutMetric(L"title", L"iconLeft", 9.0f);
    PutMetric(L"title", L"iconTop", 7.0f);
    PutMetric(L"title", L"textGap", 7.0f);
    PutMetric(L"title", L"textEnd", 134.0f);
    PutMetric(L"majorNav", L"height", 34.0f);
    PutMetric(L"majorNavItem", L"offsetX", 4.0f);
    PutMetric(L"majorNavItem", L"minWidth", 72.0f);
    PutMetric(L"majorNavItem", L"maxWidth", 128.0f);
    PutMetric(L"majorNavItem", L"widthAdjust", -14.0f);
    PutMetric(L"majorNavItem", L"textInsetX", 10.0f);
    PutMetric(L"majorNavItem", L"textMinWidth", 86.0f);
    PutMetric(L"majorNavItem", L"textMaxWidth", 168.0f);
    PutMetric(L"majorNavItem", L"textBaseWidth", 28.0f);
    PutMetric(L"majorNavItem", L"textCharWidth", 12.0f);
    PutMetric(L"majorNavItem", L"contentRightPadding", 4.0f);
    PutMetric(L"majorNavItem", L"visibilityPadding", 8.0f);
    PutMetric(L"majorNavItem", L"verticalTopInset", 2.0f);
    PutMetric(L"majorNavItem", L"verticalBottomInset", 2.0f);
    PutMetric(L"majorNavItem", L"verticalHeight", 32.0f);
    PutMetric(L"majorNavItem", L"verticalGap", 2.0f);
    PutMetric(L"minorNav", L"width", 72.0f);
    PutMetric(L"minorNav", L"minWidth", 40.0f);
    PutMetric(L"minorNav", L"maxWidthRatio", 0.42f);
    PutMetric(L"minorNavItem", L"topInset", 2.0f);
    PutMetric(L"minorNavItem", L"bottomInset", 2.0f);
    PutMetric(L"minorNavItem", L"height", 32.0f);
    PutMetric(L"minorNavItem", L"gap", 2.0f);
    PutMetric(L"minorNavItem", L"textInsetX", 10.0f);
    PutMetric(L"minorNavItem", L"accentWidth", 3.0f);
    PutMetric(L"minorNavItem", L"accentInsetY", 3.0f);
    PutMetric(L"minorNavItem", L"visibilityPadding", 8.0f);
    PutMetric(L"linkItem", L"viewportPaddingX", 16.0f);
    PutMetric(L"linkItem", L"tileMinSide", 74.0f);
    PutMetric(L"linkItem", L"tileMaxSide", 96.0f);
    PutMetric(L"linkItem", L"tileIconExtra", 42.0f);
    PutMetric(L"linkItem", L"listLeftInset", 4.0f);
    PutMetric(L"linkItem", L"listTopInset", 6.0f);
    PutMetric(L"linkItem", L"listBottomInset", 6.0f);
    PutMetric(L"linkItem", L"listGapX", 0.0f);
    PutMetric(L"linkItem", L"listGapY", 2.0f);
    PutMetric(L"linkItem", L"listMinHeight", 34.0f);
    PutMetric(L"linkItem", L"listHeightExtra", 12.0f);
    PutMetric(L"linkItem", L"gridLeftInset", 8.0f);
    PutMetric(L"linkItem", L"gridTopInset", 8.0f);
    PutMetric(L"linkItem", L"gridBottomInset", 8.0f);
    PutMetric(L"linkItem", L"gridGapX", 4.0f);
    PutMetric(L"linkItem", L"gridGapY", 4.0f);
    PutMetric(L"linkItem", L"compactGapX", 6.0f);
    PutMetric(L"linkItem", L"compactGapY", 6.0f);
    PutMetric(L"linkItem", L"compactPreferredWidth", 128.0f);
    PutMetric(L"linkItem", L"compactMinWidth", 72.0f);
    PutMetric(L"linkItem", L"selectedAccentWidth", 4.0f);
    PutMetric(L"linkItem", L"customColorWidth", 3.0f);
    PutMetric(L"linkItem", L"customColorInsetY", 4.0f);
    PutMetric(L"linkItem", L"listIconLeft", 8.0f);
    PutMetric(L"linkItem", L"listTextGap", 8.0f);
    PutMetric(L"linkItem", L"listTextRightInset", 6.0f);
    PutMetric(L"linkItem", L"gridIconTop", 8.0f);
    PutMetric(L"linkItem", L"gridTextPaddingX", 4.0f);
    PutMetric(L"linkItem", L"gridTextGap", 4.0f);
    PutMetric(L"linkItem", L"gridTextPaddingBottom", 4.0f);
    PutMetric(L"linkItem", L"smallTextWidthThreshold", 78.0f);
    PutMetric(L"linkItem", L"visibilityPadding", 8.0f);
    PutMetric(L"linkItem", L"runCountWidth", 36.0f);
    PutMetric(L"linkItem", L"runCountRightInset", 8.0f);
    PutMetric(L"linkItem", L"runCountGridWidth", 28.0f);
    PutMetric(L"linkItem", L"runCountGridHeight", 16.0f);
    PutMetric(L"linkItem", L"runCountGridRightInset", 4.0f);
    PutMetric(L"linkItem", L"runCountGridTopInset", 3.0f);
    PutMetric(L"iconFallback", L"radius", 7.0f);
    PutMetric(L"content", L"emptyTextInsetX", 20.0f);
    PutMetric(L"content", L"emptyTextTop", 24.0f);
    PutMetric(L"content", L"emptyTextHeight", 30.0f);
    PutMetric(L"text", L"textHeight", 20.0f);
    PutMetric(L"text", L"textOffsetY", 1.0f);
    PutMetric(L"label", L"height", 20.0f);
    PutMetric(L"label", L"textHeight", 20.0f);
    PutMetric(L"label", L"textOffsetY", 1.0f);
    PutMetric(L"panel", L"radius", 7.0f);
    PutMetric(L"panel", L"borderWidth", 1.0f);
    PutMetric(L"panel", L"paddingX", 10.0f);
    PutMetric(L"panel", L"paddingY", 8.0f);
    PutMetric(L"field", L"radius", 7.0f);
    PutMetric(L"field", L"borderWidth", 1.0f);
    PutMetric(L"field", L"height", 32.0f);
    PutMetric(L"field", L"paddingX", 9.0f);
    PutMetric(L"field", L"textHeight", 20.0f);
    PutMetric(L"field", L"textOffsetY", 1.0f);
    PutMetric(L"global", L"controlHeight", 30.0f);
    PutMetric(L"global", L"compactControlHeight", 24.0f);
    PutMetric(L"global", L"fieldHeight", 32.0f);
    PutMetric(L"global", L"paddingX", 10.0f);
    PutMetric(L"global", L"paddingY", 6.0f);
    PutMetric(L"global", L"itemGap", 8.0f);
    PutMetric(L"global", L"rowGap", 6.0f);
    PutMetric(L"global", L"sectionGap", 16.0f);
    PutMetric(L"scrollbar", L"thickness", 5.0f);
    PutMetric(L"scrollbar", L"inset", 5.0f);
    PutMetric(L"scrollbar", L"edgeInset", 4.0f);
    PutMetric(L"scrollbar", L"minThumbLength", 24.0f);
    PutMetric(L"scrollbar", L"wheelStepX", 72.0f);
    PutMetric(L"scrollbar", L"wheelStepY", 80.0f);
    PutMetric(L"menuItem", L"height", 30.0f);
    PutMetric(L"menuItem", L"widthBase", 54.0f);
    PutMetric(L"menuItem", L"minTextWidth", 64.0f);
    PutMetric(L"menuItem", L"maxTextWidth", 360.0f);
    PutMetric(L"menuItem", L"charWidth", 13.0f);
    PutMetric(L"menuItem", L"hoverInsetX", 4.0f);
    PutMetric(L"menuItem", L"hoverInsetY", 3.0f);
    PutMetric(L"menuItem", L"iconLeft", 8.0f);
    PutMetric(L"menuItem", L"iconInsetY", 6.0f);
    PutMetric(L"menuItem", L"iconSize", 16.0f);
    PutMetric(L"menuItem", L"textLeft", 34.0f);
    PutMetric(L"menuItem", L"textRight", 8.0f);
    PutMetric(L"menuItem", L"checkedTextRight", 28.0f);
    PutMetric(L"menuItem", L"submenuRight", 22.0f);
    PutMetric(L"menuItem", L"arrowRight", 9.0f);
    PutMetric(L"menuItem", L"arrowWidth", 5.0f);
    PutMetric(L"menuItem", L"arrowHalfHeight", 4.0f);
    PutMetric(L"menuItem", L"checkRight", 10.0f);
    PutMetric(L"menuItem", L"checkWidth", 9.0f);
    PutMetric(L"menuItem", L"checkHeight", 7.0f);
    PutMetric(L"menuItem", L"checkMarkWidth", 2.0f);
    PutMetric(L"button", L"radius", 6.0f);
    PutMetric(L"button", L"borderWidth", 1.0f);
    PutMetric(L"button", L"height", 30.0f);
    PutMetric(L"button", L"compactHeight", 24.0f);
    PutMetric(L"button", L"paddingX", 12.0f);
    PutMetric(L"button", L"textHeight", 20.0f);
    PutMetric(L"button", L"textOffsetY", 0.0f);
    PutMetric(L"button", L"pressedOffset", 1.0f);
    PutMetric(L"iconButton", L"size", 28.0f);
    PutMetric(L"iconButton", L"radius", 7.0f);
    PutMetric(L"iconButton", L"iconSize", 16.0f);
    PutMetric(L"miniButton", L"width", 26.0f);
    PutMetric(L"miniButton", L"height", 24.0f);
    PutMetric(L"miniButton", L"radius", 6.0f);
    PutMetric(L"miniButton", L"borderWidth", 1.0f);
    PutMetric(L"miniButton", L"arrowSize", 5.0f);
    PutMetric(L"miniButton", L"arrowStrokeWidth", 2.0f);
    PutMetric(L"miniButton", L"pressedOffset", 1.0f);
    PutMetric(L"edit", L"radius", 7.0f);
    PutMetric(L"edit", L"borderWidth", 1.0f);
    PutMetric(L"edit", L"height", 32.0f);
    PutMetric(L"edit", L"paddingX", 9.0f);
    PutMetric(L"edit", L"textHeight", 14.0f);
    PutMetric(L"edit", L"textOffsetY", 0.0f);
    PutMetric(L"edit", L"singleLineFontSizePx", 14.0f);
    PutMetric(L"edit", L"singleLineControlHeight", 24.0f);
    PutMetric(L"edit", L"singleLineOffsetY", 0.0f);
    PutMetric(L"edit", L"multiLinePaddingTop", 7.0f);
    PutMetric(L"edit", L"multiLinePaddingBottom", 7.0f);
    PutMetric(L"comboBox", L"radius", 7.0f);
    PutMetric(L"comboBox", L"borderWidth", 1.0f);
    PutMetric(L"comboBox", L"paddingX", 9.0f);
    PutMetric(L"comboBox", L"itemHeight", 26.0f);
    PutMetric(L"comboBox", L"textHeight", 20.0f);
    PutMetric(L"comboBox", L"textOffsetY", 1.0f);
    PutMetric(L"comboBox", L"arrowWidth", 28.0f);
    PutMetric(L"comboBox", L"arrowSize", 5.0f);
    PutMetric(L"comboBox", L"arrowPaddingRight", 11.0f);
    PutMetric(L"comboBox", L"arrowOffsetY", 1.0f);
    PutMetric(L"comboBox", L"arrowStrokeWidth", 1.0f);
    PutMetric(L"tabButton", L"radius", 8.0f);
    PutMetric(L"tabButton", L"borderWidth", 1.0f);
    PutMetric(L"tabButton", L"height", 30.0f);
    PutMetric(L"tabButton", L"paddingX", 12.0f);
    PutMetric(L"tabButton", L"minTextWidth", 18.0f);
    PutMetric(L"tabButton", L"textHeight", 20.0f);
    PutMetric(L"tabButton", L"textOffsetY", 0.0f);
    PutMetric(L"tabButton", L"segmented", 1.0f);
    PutMetric(L"tabButton", L"segmentInset", 0.5f);
    PutMetric(L"tabButton", L"groupItemWidth", 58.0f);
    PutMetric(L"tabButton", L"groupGap", 0.0f);
    PutMetric(L"tabButton", L"groupPadding", 1.0f);
    PutMetric(L"tabButton", L"groupRadius", 0.0f);
    PutMetric(L"tabButton", L"groupBorderWidth", 0.0f);
    PutMetric(L"checkbox", L"boxSize", 16.0f);
    PutMetric(L"checkbox", L"radius", 4.0f);
    PutMetric(L"checkbox", L"borderWidth", 1.0f);
    PutMetric(L"checkbox", L"gap", 8.0f);
    PutMetric(L"checkbox", L"height", 24.0f);
    PutMetric(L"checkbox", L"textHeight", 20.0f);
    PutMetric(L"checkbox", L"textOffsetY", 1.0f);
    PutMetric(L"checkbox", L"boxOffsetX", 0.0f);
    PutMetric(L"checkbox", L"boxOffsetY", 0.0f);
    PutMetric(L"checkbox", L"markWidth", 2.0f);
    PutMetric(L"toggle", L"width", 38.0f);
    PutMetric(L"toggle", L"height", 22.0f);
    PutMetric(L"toggle", L"thumbSize", 18.0f);
    PutMetric(L"radio", L"dotSize", 16.0f);
    PutMetric(L"radio", L"innerDotSize", 8.0f);
    PutMetric(L"radio", L"height", 24.0f);
    PutMetric(L"radio", L"gap", 8.0f);
    PutMetric(L"list", L"radius", 7.0f);
    PutMetric(L"list", L"borderWidth", 2.0f);
    PutMetric(L"list", L"paddingX", 8.0f);
    PutMetric(L"list", L"paddingY", 6.0f);
    PutMetric(L"listItem", L"height", 28.0f);
    PutMetric(L"listItem", L"paddingX", 8.0f);
    PutMetric(L"listItem", L"textHeight", 20.0f);
    PutMetric(L"listItem", L"textOffsetY", 1.0f);
    PutMetric(L"slider", L"height", 24.0f);
    PutMetric(L"slider", L"trackHeight", 4.0f);
    PutMetric(L"slider", L"thumbSize", 14.0f);
    PutMetric(L"tooltip", L"radius", 6.0f);
    PutMetric(L"tooltip", L"borderWidth", 1.0f);
    PutMetric(L"tooltip", L"paddingX", 8.0f);
    PutMetric(L"tooltip", L"paddingY", 7.0f);
    PutMetric(L"tooltip", L"lineGap", 4.0f);
    PutMetric(L"separator", L"thickness", 1.0f);
    PutMetric(L"separator", L"inset", 0.0f);
}

void Theme::ParseXml(const std::wstring& xml) {
    if (xml.find(L"<Theme") == std::wstring::npos) {
        return;
    }

    const std::wstring palette = FindBlock(xml, L"Palette");
    std::size_t pos = 0;
    while ((pos = palette.find(L"<Color", pos)) != std::wstring::npos) {
        const std::size_t end = palette.find(L">", pos);
        if (end == std::wstring::npos) {
            break;
        }
        const std::wstring tag = palette.substr(pos, end - pos + 1);
        const std::wstring name = AttributeValue(tag, L"name");
        const std::wstring value = AttributeValue(tag, L"value");
        if (!name.empty() && !value.empty()) {
            rawPalette_[name] = value;
        }
        pos = end + 1;
    }

    pos = 0;
    while ((pos = xml.find(L"<Component", pos)) != std::wstring::npos) {
        const std::size_t tagEnd = xml.find(L">", pos);
        if (tagEnd == std::wstring::npos) {
            break;
        }
        const std::wstring componentTag = xml.substr(pos, tagEnd - pos + 1);
        const std::wstring component = AttributeValue(componentTag, L"name");
        const std::size_t close = xml.find(L"</Component>", tagEnd + 1);
        if (close == std::wstring::npos) {
            break;
        }
        if (!component.empty() && !IsAllowedComponent(component)) {
            ReportUnsupportedComponent(component);
        } else if (!component.empty()) {
            const std::wstring body = xml.substr(tagEnd + 1, close - tagEnd - 1);
            std::size_t child = 0;
            while ((child = body.find(L"<State", child)) != std::wstring::npos) {
                const std::size_t childEnd = body.find(L">", child);
                if (childEnd == std::wstring::npos) {
                    break;
                }
                const auto attrs = Attributes(body.substr(child, childEnd - child + 1));
                auto state = attrs.find(L"name");
                if (state != attrs.end() && !state->second.empty()) {
                    for (const auto& [role, value] : attrs) {
                        if (role != L"name" && !value.empty()) {
                            rawStateColors_[Key(component, state->second, role)] = value;
                        }
                    }
                }
                child = childEnd + 1;
            }

            child = 0;
            while ((child = body.find(L"<Metric", child)) != std::wstring::npos) {
                const std::size_t childEnd = body.find(L">", child);
                if (childEnd == std::wstring::npos) {
                    break;
                }
                const std::wstring tag = body.substr(child, childEnd - child + 1);
                const std::wstring name = AttributeValue(tag, L"name");
                const std::wstring value = AttributeValue(tag, L"value");
                if (!name.empty()) {
                    if (auto parsed = ParseFloatValue(value)) {
                        metrics_[Key(component, name)] = *parsed;
                    }
                }
                child = childEnd + 1;
            }
        }
        pos = close + 12;
    }
}

Color Theme::ResolveColorValue(const std::wstring& value, int depth) const {
    if (depth > 8) {
        return color(L"global", L"normal", L"text");
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
        auto color = palette_.find(ref);
        if (color != palette_.end()) {
            return color->second;
        }
        auto raw = rawPalette_.find(ref);
        if (raw != rawPalette_.end()) {
            return ResolveColorValue(raw->second, depth + 1);
        }
    }

    return color(L"global", L"normal", L"text");
}

void Theme::PutPalette(const std::wstring& name, Color color) {
    palette_[name] = color;
}

void Theme::PutState(const std::wstring& component, const std::wstring& state, const std::wstring& role, Color color) {
    stateColors_[Key(component, state, role)] = color;
}

void Theme::PutMetric(const std::wstring& component, const std::wstring& name, float value) {
    metrics_[Key(component, name)] = value;
}
