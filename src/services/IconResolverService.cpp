#include "IconResolverService.h"

#include "AppLog.h"
#include "MenuCatalog.h"
#include "ShellItemService.h"
#include "SystemFunctions.h"
#include "TerminalContextMenuService.h"
#include "ThemedUi.h"
#include "Utilities.h"

#include <appmodel.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

#include <pugixml.hpp>

namespace {

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

class ScopedComInitialization final {
public:
    ScopedComInitialization() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedComInitialization() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    bool usable() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT result_ = E_FAIL;
};

bool ContainsStraightAlphaPixels(const std::vector<std::uint32_t>& pixels) {
    return std::any_of(pixels.begin(), pixels.end(), [](std::uint32_t pixel) {
        const std::uint32_t alpha = pixel >> 24;
        if (alpha == 0 || alpha == 255) {
            return false;
        }
        const std::uint32_t blue = pixel & 0xFFu;
        const std::uint32_t green = (pixel >> 8) & 0xFFu;
        const std::uint32_t red = (pixel >> 16) & 0xFFu;
        return red > alpha || green > alpha || blue > alpha;
    });
}

void PremultiplyTranslucentPixels(std::vector<std::uint32_t>& pixels) {
    for (auto& pixel : pixels) {
        const std::uint32_t alpha = pixel >> 24;
        if (alpha == 0 || alpha == 255) {
            continue;
        }
        const std::uint32_t blue = pixel & 0xFFu;
        const std::uint32_t green = (pixel >> 8) & 0xFFu;
        const std::uint32_t red = (pixel >> 16) & 0xFFu;
        pixel = (alpha << 24) |
                (((red * alpha + 127u) / 255u) << 16) |
                (((green * alpha + 127u) / 255u) << 8) |
                ((blue * alpha + 127u) / 255u);
    }
}

constexpr wchar_t kResolverCacheNamespace[] = L"resolver-v5";

std::wstring HashBytesHex(const void* data, std::size_t byteCount) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    const bool hashed = byteCount == 0 ||
        BCryptHashData(
            hash,
            reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
            static_cast<ULONG>(std::min<std::size_t>(byteCount, std::numeric_limits<ULONG>::max())),
            0) == 0;
    const bool finished = hashed &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!finished) {
        return {};
    }
    std::wstringstream stream;
    stream << std::hex << std::setfill(L'0');
    for (const unsigned char byte : digest) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

std::wstring HashWideHex(const std::wstring& value) {
    return HashBytesHex(value.data(), value.size() * sizeof(wchar_t));
}

std::wstring HashBytesHex(const std::vector<std::uint8_t>& value) {
    return HashBytesHex(value.data(), value.size());
}

std::wstring KindKey(IconSourceKind kind) {
    switch (kind) {
    case IconSourceKind::Link: return L"link";
    case IconSourceKind::FilePath: return L"file";
    case IconSourceKind::DirectoryPath: return L"directory";
    case IconSourceKind::Url: return L"url";
    case IconSourceKind::ShellParseName: return L"shell-parse-name";
    case IconSourceKind::PidlBlob: return L"pidl";
    case IconSourceKind::IconLocation: return L"icon-location";
    case IconSourceKind::CommandLine: return L"command";
    case IconSourceKind::ContextMenuProvider: return L"context-menu-provider";
    case IconSourceKind::Stock: return L"stock";
    case IconSourceKind::DefaultCategory: return L"default-category";
    default: return L"unknown";
    }
}

bool LooksLikeUrl(const Link& link) {
    const std::wstring lower = ToLower(Trim(link.path));
    return link.type == 2 ||
           lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"ftp://", 0) == 0 ||
           lower.rfind(L"www.", 0) == 0;
}

std::wstring UrlHost(const std::wstring& value) {
    std::wstring text = NormalizeUrl(value);
    const std::size_t scheme = text.find(L"://");
    if (scheme != std::wstring::npos) {
        text.erase(0, scheme + 3);
    }
    const std::size_t end = text.find_first_of(L"/?#");
    if (end != std::wstring::npos) {
        text.resize(end);
    }
    const std::size_t at = text.rfind(L'@');
    if (at != std::wstring::npos) {
        text.erase(0, at + 1);
    }
    if (!text.empty() && text.front() == L'[') {
        const std::size_t close = text.find(L']');
        if (close != std::wstring::npos) {
            return ToLower(text.substr(0, close + 1));
        }
    }
    const std::size_t port = text.rfind(L':');
    if (port != std::wstring::npos) {
        text.resize(port);
    }
    return ToLower(Trim(text));
}

std::filesystem::path UrlIconFile(
    const std::filesystem::path& appDirectory,
    const Link& link,
    bool preferPng) {
    const std::wstring host = UrlHost(link.path);
    if (appDirectory.empty() || host.empty()) {
        return {};
    }
    const std::filesystem::path iconDir = appDirectory / L"icons" / L"url";
    const std::array<std::filesystem::path, 4> candidates = preferPng
        ? std::array<std::filesystem::path, 4>{
            iconDir / (host + L".png"),
            iconDir / (ToLower(host) + L".png"),
            iconDir / (host + L".ico"),
            iconDir / (ToLower(host) + L".ico"),
        }
        : std::array<std::filesystem::path, 4>{
            iconDir / (host + L".ico"),
            iconDir / (ToLower(host) + L".ico"),
            iconDir / (host + L".png"),
            iconDir / (ToLower(host) + L".png"),
        };
    for (const auto& candidate : candidates) {
        if (FileExists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    return result;
}

std::wstring NodeLocalName(const char* name) {
    if (!name) {
        return {};
    }
    const char* local = std::strrchr(name, ':');
    return Utf8ToWide(local ? local + 1 : name);
}

bool NodeIs(const pugi::xml_node& node, const wchar_t* localName) {
    return NodeLocalName(node.name()) == localName;
}

std::optional<std::wstring> PackageFamilyFromAumid(const std::wstring& value) {
    const std::wstring trimmed = Trim(value);
    const auto bang = trimmed.find(L'!');
    if (bang == std::wstring::npos || bang == 0) {
        return std::nullopt;
    }
    const std::wstring family = trimmed.substr(0, bang);
    if (family.find_first_of(L"\\/:") != std::wstring::npos) {
        return std::nullopt;
    }
    return family;
}

std::optional<std::filesystem::path> PackageInstallPath(const std::wstring& packageFamily) {
    UINT32 count = 0;
    UINT32 bufferLength = 0;
    LONG rc = GetPackagesByPackageFamily(
        packageFamily.c_str(),
        &count,
        nullptr,
        &bufferLength,
        nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0 || bufferLength == 0) {
        return std::nullopt;
    }

    std::vector<PWSTR> packageNames(count);
    std::vector<wchar_t> packageNameBuffer(bufferLength);
    rc = GetPackagesByPackageFamily(
        packageFamily.c_str(),
        &count,
        packageNames.data(),
        &bufferLength,
        packageNameBuffer.data());
    if (rc != ERROR_SUCCESS || count == 0) {
        return std::nullopt;
    }

    for (UINT32 index = 0; index < count; ++index) {
        if (!packageNames[index] || !*packageNames[index]) {
            continue;
        }
        UINT32 pathLength = 0;
        rc = GetPackagePathByFullName(packageNames[index], &pathLength, nullptr);
        if (rc != ERROR_INSUFFICIENT_BUFFER || pathLength == 0) {
            continue;
        }
        std::vector<wchar_t> path(pathLength);
        rc = GetPackagePathByFullName(packageNames[index], &pathLength, path.data());
        if (rc == ERROR_SUCCESS && pathLength > 0 && path.front() != L'\0') {
            return std::filesystem::path(path.data());
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> AppxVisualElementLogo(const std::filesystem::path& manifestPath) {
    pugi::xml_document document;
    if (!document.load_file(manifestPath.c_str())) {
        return std::nullopt;
    }
    pugi::xml_node package = document.document_element();
    for (pugi::xml_node applications : package.children()) {
        if (!NodeIs(applications, L"Applications")) {
            continue;
        }
        for (pugi::xml_node application : applications.children()) {
            if (!NodeIs(application, L"Application")) {
                continue;
            }
            for (pugi::xml_node visual : application.children()) {
                if (!NodeIs(visual, L"VisualElements")) {
                    continue;
                }
                const char* logo = visual.attribute("Square44x44Logo").value();
                if (!logo || !*logo) {
                    logo = visual.attribute("Square150x150Logo").value();
                }
                if (logo && *logo) {
                    return Utf8ToWide(logo);
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<int> AppxTargetSizesByPreference(int requestedSize) {
    std::vector<int> sizes{16, 20, 24, 30, 32, 36, 40, 44, 48, 60, 64, 72, 80, 96, 256};
    sizes.push_back(std::clamp(requestedSize, 1, 256));
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    std::sort(sizes.begin(), sizes.end(), [requestedSize](int left, int right) {
        const int leftDistance = std::abs(left - requestedSize);
        const int rightDistance = std::abs(right - requestedSize);
        if (leftDistance != rightDistance) {
            return leftDistance < rightDistance;
        }
        return left > right;
    });
    return sizes;
}

void AppendUniquePath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    const std::wstring key = ToLower(path.wstring());
    const auto duplicate = std::any_of(paths.begin(), paths.end(), [&](const std::filesystem::path& existing) {
        return ToLower(existing.wstring()) == key;
    });
    if (!duplicate) {
        paths.push_back(path);
    }
}

std::vector<std::filesystem::path> AppxLogoCandidates(
    const std::filesystem::path& packageRoot,
    const std::wstring& manifestLogo,
    int requestedSize) {
    std::vector<std::filesystem::path> result;
    const std::filesystem::path relativeLogo = std::filesystem::path(manifestLogo);
    const std::filesystem::path base = packageRoot / relativeLogo;
    const std::filesystem::path directory = base.parent_path();
    const std::wstring stem = base.stem().wstring();
    const std::wstring extension = base.extension().empty() ? L".png" : base.extension().wstring();

    for (const int size : AppxTargetSizesByPreference(requestedSize)) {
        AppendUniquePath(result, directory / (stem + L".targetsize-" + std::to_wstring(size) + L"_altform-lightunplated" + extension));
        AppendUniquePath(result, directory / (stem + L".targetsize-" + std::to_wstring(size) + L"_altform-unplated" + extension));
    }
    for (const int size : AppxTargetSizesByPreference(requestedSize)) {
        AppendUniquePath(result, directory / (stem + L".targetsize-" + std::to_wstring(size) + extension));
    }
    AppendUniquePath(result, base);
    AppendUniquePath(result, directory / (stem + L".scale-200" + extension));
    return result;
}

ResolvedIcon LoadPngIcon(const std::filesystem::path& path, const std::wstring& source) {
    ScopedComInitialization com;
    if (!com.usable()) {
        return {};
    }
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ResolvedIcon result;

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        !factory) {
        return result;
    }

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(frame->GetSize(&width, &height)) &&
        width > 0 && height > 0 && width <= 1024 && height <= 1024 &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut))) {
        result.width = static_cast<int>(width);
        result.height = static_cast<int>(height);
        result.quality = 3;
        result.pixels.resize(static_cast<std::size_t>(width) * height);
        const UINT stride = width * sizeof(std::uint32_t);
        const UINT bytes = static_cast<UINT>(result.pixels.size() * sizeof(std::uint32_t));
        if (SUCCEEDED(converter->CopyPixels(nullptr, stride, bytes, reinterpret_cast<BYTE*>(result.pixels.data())))) {
            result.ok = std::any_of(result.pixels.begin(), result.pixels.end(), [](std::uint32_t pixel) {
                return pixel != 0;
            });
            result.source = source;
        } else {
            result = {};
        }
    }

    SafeRelease(converter);
    SafeRelease(frame);
    SafeRelease(decoder);
    SafeRelease(factory);
    return result;
}

std::wstring RegistryString(HKEY root, const std::wstring& subkey, const wchar_t* valueName) {
    DWORD size = 0;
    if (RegGetValueW(
            root,
            subkey.c_str(),
            valueName,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            nullptr,
            &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(
            root,
            subkey.c_str(),
            valueName,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            value.data(),
            &size) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(wcsnlen_s(value.c_str(), value.size()));
    return ExpandEnvironmentStringsSafe(Trim(value));
}

std::wstring ResolveExecutablePath(std::wstring path);
std::wstring ExistingExecutableCandidate(const std::wstring& value);

std::wstring ExecutableFromCommand(const std::wstring& command) {
    std::wstring trimmed = ExpandEnvironmentStringsSafe(Trim(command));
    if (trimmed.empty()) {
        return {};
    }

    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(trimmed.c_str(), &count);
    std::wstring firstArgument;
    if (arguments && count > 0) {
        firstArgument = arguments[0];
    }
    if (arguments) {
        LocalFree(arguments);
    }

    if (!firstArgument.empty()) {
        if (const std::wstring resolved = ExistingExecutableCandidate(firstArgument); !resolved.empty()) {
            return resolved;
        }
    }

    if (trimmed.front() == L'"') {
        const std::size_t end = trimmed.find(L'"', 1);
        if (end != std::wstring::npos) {
            const std::wstring quoted = trimmed.substr(1, end - 1);
            if (const std::wstring resolved = ExistingExecutableCandidate(quoted); !resolved.empty()) {
                return resolved;
            }
            return ExpandEnvironmentStringsSafe(Trim(quoted));
        }
    }

    std::vector<std::wstring> candidates;
    for (std::size_t index = 0; index < trimmed.size(); ++index) {
        if (!std::iswspace(trimmed[index])) continue;
        std::wstring candidate = Trim(trimmed.substr(0, index));
        if (!candidate.empty()) candidates.push_back(std::move(candidate));
    }
    if (candidates.empty()) {
        candidates.push_back(trimmed);
    } else if (std::find(candidates.begin(), candidates.end(), trimmed) == candidates.end()) {
        candidates.push_back(trimmed);
    }
    for (const std::wstring& candidate : candidates) {
        if (const std::wstring resolved = ExistingExecutableCandidate(candidate); !resolved.empty()) {
            return resolved;
        }
    }
    return ExpandEnvironmentStringsSafe(Trim(firstArgument.empty() ? candidates.front() : firstArgument));
}

bool ParseIconLocation(std::wstring value, std::wstring& path, int& index) {
    value = Trim(value);
    if (value.empty()) {
        return false;
    }
    if (value.front() == L'@') {
        value.erase(value.begin());
    }
    index = 0;
    const auto comma = value.rfind(L',');
    if (comma != std::wstring::npos) {
        if (const auto parsed = ParseInt(Trim(value.substr(comma + 1)))) {
            index = *parsed;
            value.resize(comma);
        }
    }
    value = Trim(value);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    path = ExpandEnvironmentStringsSafe(Trim(value));
    return !path.empty();
}

std::wstring ResolveExecutablePath(std::wstring path) {
    path = ExpandEnvironmentStringsSafe(Trim(path));
    if (path.empty()) {
        return {};
    }
    const std::wstring lowerPath = ToLower(path);
    if (lowerPath.rfind(L"\\systemroot\\", 0) == 0 || lowerPath.rfind(L"systemroot\\", 0) == 0) {
        wchar_t windows[MAX_PATH]{};
        if (GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)))) {
            const std::wstring suffix = path.substr(path.front() == L'\\' ? 12 : 11);
            path = (std::filesystem::path(windows) / suffix).wstring();
        }
    } else if (lowerPath.rfind(L"system32\\", 0) == 0 || lowerPath.rfind(L"syswow64\\", 0) == 0) {
        wchar_t windows[MAX_PATH]{};
        if (GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)))) {
            path = (std::filesystem::path(windows) / path).wstring();
        }
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        return path;
    }
    const std::wstring fileName = std::filesystem::path(path).filename().wstring();
    if (fileName.empty()) {
        return path;
    }
    const std::array<std::pair<HKEY, const wchar_t*>, 2> appPathRoots{{
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"},
        {HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\"},
    }};
    for (const auto& [root, prefix] : appPathRoots) {
        const std::wstring registered = RegistryString(root, std::wstring(prefix) + fileName, nullptr);
        if (!registered.empty() && std::filesystem::is_regular_file(registered, ec)) {
            return registered;
        }
    }
    std::array<wchar_t, 32768> found{};
    const DWORD length = SearchPathW(
        nullptr,
        fileName.c_str(),
        nullptr,
        static_cast<DWORD>(found.size()),
        found.data(),
        nullptr);
    return length > 0 && length < found.size() ? std::wstring(found.data(), length) : path;
}

std::optional<TrackedContextMenuProviderBinding> FindProvider(const std::wstring& providerId) {
    for (const auto& provider : TrackedContextMenuProviders()) {
        if (providerId == (provider.providerId ? provider.providerId : L"")) {
            return provider;
        }
    }
    return std::nullopt;
}

HICON DuplicateIconHandle(HICON icon) {
    return icon ? CopyIcon(icon) : nullptr;
}

std::wstring ExistingExecutableCandidate(const std::wstring& value) {
    const std::wstring resolved = ResolveExecutablePath(value);
    std::error_code ec;
    return std::filesystem::is_regular_file(resolved, ec) ? resolved : std::wstring{};
}

std::wstring IconFallbackKindKey(IconFallbackKind kind) {
    switch (kind) {
    case IconFallbackKind::Application: return L"application";
    case IconFallbackKind::File: return L"file";
    case IconFallbackKind::Directory: return L"directory";
    case IconFallbackKind::Url: return L"url";
    case IconFallbackKind::Registry: return L"registry";
    case IconFallbackKind::StartupFolder: return L"startup-folder";
    case IconFallbackKind::ScheduledTask: return L"scheduled-task";
    case IconFallbackKind::Service: return L"service";
    case IconFallbackKind::Driver: return L"driver";
    case IconFallbackKind::ActiveSetup: return L"active-setup";
    case IconFallbackKind::Wmi: return L"wmi";
    case IconFallbackKind::Login: return L"login";
    case IconFallbackKind::Dll: return L"dll";
    case IconFallbackKind::ShellExtension: return L"shell-extension";
    case IconFallbackKind::System: return L"system";
    default: return L"application";
    }
}

TablerIconId FallbackTablerIcon(IconFallbackKind kind) {
    switch (kind) {
    case IconFallbackKind::File: return TablerIconId::File;
    case IconFallbackKind::Directory:
    case IconFallbackKind::StartupFolder: return TablerIconId::Folder;
    case IconFallbackKind::Url: return TablerIconId::Url;
    case IconFallbackKind::Registry: return TablerIconId::Windows;
    case IconFallbackKind::ScheduledTask: return TablerIconId::Clock;
    case IconFallbackKind::Service: return TablerIconId::Settings;
    case IconFallbackKind::Driver: return TablerIconId::Shield;
    case IconFallbackKind::ActiveSetup: return TablerIconId::Run;
    case IconFallbackKind::Wmi:
    case IconFallbackKind::System: return TablerIconId::System;
    case IconFallbackKind::Login: return TablerIconId::User;
    case IconFallbackKind::Dll: return TablerIconId::File;
    case IconFallbackKind::ShellExtension: return TablerIconId::Tools;
    case IconFallbackKind::Application:
    default: return TablerIconId::Run;
    }
}

COLORREF FallbackIconColor(IconFallbackKind kind) {
    switch (kind) {
    case IconFallbackKind::Driver: return RGB(37, 99, 235);
    case IconFallbackKind::Service: return RGB(59, 130, 246);
    case IconFallbackKind::ScheduledTask: return RGB(14, 165, 233);
    case IconFallbackKind::Registry: return RGB(2, 132, 199);
    case IconFallbackKind::StartupFolder: return RGB(217, 119, 6);
    case IconFallbackKind::ActiveSetup: return RGB(22, 163, 74);
    case IconFallbackKind::Wmi:
    case IconFallbackKind::ShellExtension:
    case IconFallbackKind::System: return RGB(75, 85, 99);
    default: return RGB(31, 41, 55);
    }
}

bool IsGenericHostExecutable(const std::wstring& executable) {
    const std::wstring file = ToLower(std::filesystem::path(executable).filename().wstring());
    return file == L"svchost.exe" || file == L"rundll32.exe" || file == L"regsvr32.exe" ||
        file == L"cmd.exe" || file == L"powershell.exe" || file == L"pwsh.exe" ||
        file == L"wscript.exe" || file == L"cscript.exe" || file == L"msiexec.exe" ||
        file == L"sc.exe";
}

bool IsScriptLikeTarget(const std::wstring& executable) {
    const std::wstring extension = ToLower(std::filesystem::path(executable).extension().wstring());
    return extension == L".bat" || extension == L".cmd" || extension == L".ps1" ||
        extension == L".vbs" || extension == L".js" || extension == L".wsf";
}

std::wstring FileFingerprint(std::wstring value, bool directory) {
    value = directory ? ExpandEnvironmentStringsSafe(Trim(value)) : ResolveExecutablePath(value);
    if (Trim(value).empty()) {
        return L"missing:";
    }
    std::error_code ec;
    std::filesystem::path path(value);
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec && !canonical.empty()) {
        path = canonical;
    }
    const std::wstring normalized = ToLower(path.wstring());
    ec.clear();
    const bool exists = directory
        ? std::filesystem::is_directory(path, ec)
        : std::filesystem::is_regular_file(path, ec);
    std::wstringstream stream;
    stream << normalized << L"|exists=" << (exists ? 1 : 0);
    if (exists) {
        ec.clear();
        const auto writeTime = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            stream << L"|mtime=" << writeTime.time_since_epoch().count();
        }
        if (!directory) {
            ec.clear();
            const auto size = std::filesystem::file_size(path, ec);
            if (!ec) {
                stream << L"|size=" << size;
            }
        }
    }
    return stream.str();
}

void AppendFileFingerprint(
    std::wstringstream& stream,
    const wchar_t* label,
    const std::wstring& value,
    bool directory) {
    if (Trim(value).empty()) {
        return;
    }
    stream << L"|" << label << L"=" << FileFingerprint(value, directory);
}

std::wstring IconRequestCacheIdentity(
    const IconRequest& request,
    int size,
    const std::filesystem::path& appDirectory) {
    std::wstringstream stream;
    stream << L"version=" << kResolverCacheNamespace
           << L"|kind=" << KindKey(request.kind)
           << L"|size=" << size
           << L"|fallback=" << IconFallbackKindKey(request.fallbackKind)
           << L"|stock=" << static_cast<int>(request.stockIcon)
           << L"|allowFallback=" << (request.allowFallback ? 1 : 0)
           << L"|genericFallback=" << (request.preferFallbackForGenericHost ? 1 : 0);

    switch (request.kind) {
    case IconSourceKind::Link: {
        stream << L"|linkType=" << request.link.type
               << L"|linkPath=" << ToLower(ExpandEnvironmentStringsSafe(Trim(request.link.path)))
               << L"|linkIcon=" << ToLower(ExpandEnvironmentStringsSafe(Trim(request.link.icon)))
               << L"|linkPidl=" << HashBytesHex(request.link.pidl);
        if (LooksLikeUrl(request.link)) {
            const std::filesystem::path urlIcon = UrlIconFile(appDirectory, request.link, true);
            stream << L"|urlHost=" << UrlHost(request.link.path)
                   << L"|urlIcon=" << ToLower(urlIcon.wstring());
            AppendFileFingerprint(stream, L"urlIconFile", urlIcon.wstring(), false);
        }
        AppendFileFingerprint(stream, L"linkPathFile", request.link.path, request.link.type == 1);
        if (!Trim(request.link.icon).empty() && request.link.icon != L"#url" &&
            request.link.icon != L"默认系统缓存图标") {
            std::wstring iconPath;
            int iconIndex = 0;
            if (ParseIconLocation(request.link.icon, iconPath, iconIndex)) {
                stream << L"|linkIconIndex=" << iconIndex;
                AppendFileFingerprint(stream, L"linkIconFile", iconPath, false);
            } else {
                AppendFileFingerprint(stream, L"linkIconFile", request.link.icon, false);
            }
        }
        break;
    }
    case IconSourceKind::FilePath:
        stream << L"|value=" << ToLower(ExpandEnvironmentStringsSafe(Trim(request.value)));
        AppendFileFingerprint(stream, L"file", request.value, false);
        break;
    case IconSourceKind::DirectoryPath:
        stream << L"|value=" << ToLower(ExpandEnvironmentStringsSafe(Trim(request.value)));
        AppendFileFingerprint(stream, L"directory", request.value, true);
        break;
    case IconSourceKind::IconLocation: {
        std::wstring iconPath;
        int iconIndex = 0;
        ParseIconLocation(request.value, iconPath, iconIndex);
        stream << L"|value=" << ToLower(ExpandEnvironmentStringsSafe(Trim(request.value)))
               << L"|iconIndex=" << iconIndex;
        AppendFileFingerprint(stream, L"iconFile", iconPath, false);
        break;
    }
    case IconSourceKind::CommandLine: {
        const std::wstring executable = ExecutableFromCommand(request.value);
        stream << L"|command=" << ToLower(ExpandEnvironmentStringsSafe(Trim(request.value)))
               << L"|executable=" << ToLower(executable);
        AppendFileFingerprint(stream, L"commandFile", executable, false);
        break;
    }
    case IconSourceKind::PidlBlob:
        stream << L"|pidl=" << HashBytesHex(request.pidl);
        break;
    case IconSourceKind::ContextMenuProvider:
        stream << L"|provider=" << ToLower(Trim(request.providerId));
        break;
    case IconSourceKind::ShellParseName:
    case IconSourceKind::Url:
        stream << L"|value=" << ToLower(ExpandEnvironmentStringsSafe(Trim(request.value)));
        break;
    case IconSourceKind::Stock:
    case IconSourceKind::DefaultCategory:
    default:
        break;
    }
    return stream.str();
}

std::wstring LogText(std::wstring value, std::size_t maxLength = 180) {
    value = Trim(value);
    for (wchar_t& ch : value) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            ch = L' ';
        }
    }
    if (value.size() <= maxLength) {
        return value;
    }
    return value.substr(0, maxLength - 1) + L"…";
}

std::wstring CacheModeKey(IconCacheMode mode) {
    switch (mode) {
    case IconCacheMode::PreferCache: return L"prefer-cache";
    case IconCacheMode::Refresh: return L"refresh";
    case IconCacheMode::Bypass: return L"bypass";
    case IconCacheMode::Disabled: return L"disabled";
    default: return L"unknown";
    }
}

std::wstring IconRouteFromSource(const std::wstring& source) {
    if (source.rfind(L"disk-cache:", 0) == 0) return L"disk-cache";
    if (source == L"url-icon-file") return L"url-icon-file";
    if (source.rfind(L"shell-item-image", 0) == 0) return L"shell-item-image";
    if (source.rfind(L"appx-unplated", 0) != std::wstring::npos) return L"appx-unplated";
    if (source.rfind(L"fallback-", 0) == 0) return L"semantic-fallback";
    if (source == L"icon-location") return L"icon-location";
    if (source == L"file" || source == L"file-attributes") return L"file";
    if (source == L"directory" || source == L"directory-attributes") return L"directory";
    if (source == L"pidl") return L"pidl";
    if (source == L"shell-parse-name") return L"shell-parse-name";
    if (source == L"stock" || source == L"default-application") return L"stock";
    if (source == L"tabler-system") return L"tabler-system";
    if (source == L"context-menu-provider") return L"context-menu-provider";
    if (source == L"terminal-executable") return L"terminal-executable";
    return source.empty() ? L"unknown" : source;
}

std::wstring IconRequestLogTarget(const IconRequest& request) {
    switch (request.kind) {
    case IconSourceKind::Link:
        return L"name=\"" + LogText(request.link.name, 80) + L"\", path=\"" +
            LogText(request.link.path) + L"\", icon=\"" + LogText(request.link.icon, 100) + L"\"";
    case IconSourceKind::FilePath:
    case IconSourceKind::DirectoryPath:
    case IconSourceKind::IconLocation:
    case IconSourceKind::CommandLine:
    case IconSourceKind::ShellParseName:
    case IconSourceKind::Url:
        return L"value=\"" + LogText(request.value) + L"\"";
    case IconSourceKind::ContextMenuProvider:
        return L"provider=\"" + LogText(request.providerId, 100) + L"\"";
    case IconSourceKind::PidlBlob:
        return L"pidlHash=" + HashBytesHex(request.pidl).substr(0, 16);
    case IconSourceKind::Stock:
    case IconSourceKind::DefaultCategory:
    default:
        return {};
    }
}

void LogIconResolve(
    const IconRequest& request,
    int size,
    const std::wstring& route,
    const std::wstring& source,
    const std::filesystem::path& cachePath,
    bool cacheWritten,
    bool success) {
    if (!IsAppLogInitialized() || !IsAppLogEnabled()) {
        return;
    }
    std::wstring line = success ? L"图标加载成功" : L"图标加载失败";
    line += L": route=" + (route.empty() ? L"unknown" : route);
    line += L", source=" + (source.empty() ? L"(none)" : LogText(source, 120));
    line += L", kind=" + KindKey(request.kind);
    line += L", size=" + std::to_wstring(size);
    line += L", cacheMode=" + CacheModeKey(request.cacheMode);
    line += L", fallback=" + IconFallbackKindKey(request.fallbackKind);
    if (!cachePath.empty()) {
        line += L", cacheFile=" + cachePath.filename().wstring();
    }
    if (cacheWritten) {
        line += L", cacheWrite=1";
    }
    const std::wstring target = IconRequestLogTarget(request);
    if (!target.empty()) {
        line += L", " + target;
    }
    WriteAppLog(line);
}

}

IconResolverService::IconResolverService(
    std::filesystem::path appDirectory,
    std::filesystem::path cacheDirectory)
    : appDirectory_(std::move(appDirectory)),
      cacheDirectory_(std::move(cacheDirectory)) {}

IconRequest IconResolverService::ForLink(const Link& link, int size) {
    IconRequest request;
    request.kind = IconSourceKind::Link;
    request.size = size;
    request.link = link;
    return request;
}

IconRequest IconResolverService::ForPidl(std::vector<std::uint8_t> pidl, int size) {
    IconRequest request;
    request.kind = IconSourceKind::PidlBlob;
    request.size = size;
    request.pidl = std::move(pidl);
    return request;
}

IconRequest IconResolverService::ForContextMenuProvider(std::wstring providerId, int size) {
    IconRequest request;
    request.kind = IconSourceKind::ContextMenuProvider;
    request.size = size;
    request.providerId = std::move(providerId);
    return request;
}

bool IconResolverService::HasPixels(const ResolvedIcon& icon) {
    return icon.ok && icon.width > 0 && icon.height > 0 &&
        icon.pixels.size() == static_cast<std::size_t>(icon.width * icon.height);
}

std::filesystem::path IconResolverService::DefaultCacheDirectory(const std::filesystem::path& appDirectory) {
    const std::filesystem::path root = appDirectory.empty() ? QuattroUserConfigDirectory() : appDirectory;
    return root / L"cache" / L"icons" / kResolverCacheNamespace;
}

ResolvedIcon IconResolverService::LoadPngIconFile(
    const std::filesystem::path& path,
    const std::wstring& source) {
    return LoadPngIcon(path, source);
}

bool IconResolverService::SavePngIcon(const ResolvedIcon& icon, const std::filesystem::path& path) {
    if (!HasPixels(icon) || path.empty()) {
        return false;
    }
    ScopedComInitialization com;
    if (!com.usable()) {
        return false;
    }
    IWICImagingFactory* factory = nullptr;
    IWICBitmap* bitmap = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    const UINT width = static_cast<UINT>(icon.width);
    const UINT height = static_cast<UINT>(icon.height);
    const UINT stride = static_cast<UINT>(icon.width * sizeof(std::uint32_t));
    const UINT bytes = static_cast<UINT>(icon.pixels.size() * sizeof(std::uint32_t));

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }
    std::filesystem::path tempPath = path;
    tempPath += L".tmp";
    std::filesystem::remove(tempPath, ec);

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateBitmapFromMemory(
            width,
            height,
            GUID_WICPixelFormat32bppBGRA,
            stride,
            bytes,
            reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(icon.pixels.data())),
            &bitmap)) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(tempPath.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr))) {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(width, height)) &&
            SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(frame->WriteSource(bitmap, nullptr)) &&
            SUCCEEDED(frame->Commit()) &&
            SUCCEEDED(encoder->Commit());
    }

    SafeRelease(frame);
    SafeRelease(encoder);
    SafeRelease(stream);
    SafeRelease(bitmap);
    SafeRelease(factory);
    if (!ok) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    if (MoveFileExW(
            tempPath.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    ec.clear();
    std::filesystem::copy_file(tempPath, path, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(tempPath, ec);
    return !ec;
}

bool IconResolverService::ClearDiskCache(
    const std::filesystem::path& appDirectory,
    const std::filesystem::path& cacheDirectory) {
    const std::filesystem::path cacheRoot = cacheDirectory.empty()
        ? DefaultCacheDirectory(appDirectory)
        : cacheDirectory;
    std::error_code ec;
    if (std::filesystem::exists(cacheRoot, ec)) {
        std::filesystem::remove_all(cacheRoot, ec);
        if (ec) {
            return false;
        }
    }
    std::filesystem::create_directories(cacheRoot, ec);
    return !ec;
}

std::filesystem::path IconResolverService::CacheRoot() const {
    return cacheDirectory_.empty() ? DefaultCacheDirectory(appDirectory_) : cacheDirectory_;
}

std::filesystem::path IconResolverService::CachePathForRequest(const IconRequest& request, int size) const {
    const std::wstring hash = HashWideHex(IconRequestCacheIdentity(request, size, appDirectory_));
    if (hash.size() < 2) {
        return {};
    }
    return CacheRoot() / hash.substr(0, 2) / (hash + L".png");
}

ResolvedIcon IconResolverService::Resolve(const IconRequest& request, std::stop_token stopToken) const {
    if (stopToken.stop_requested()) {
        return {};
    }
    const int size = std::clamp(request.size, 1, 256);
    const bool cacheEnabled = request.cacheMode == IconCacheMode::PreferCache ||
        request.cacheMode == IconCacheMode::Refresh;
    const std::filesystem::path cachePath = cacheEnabled
        ? CachePathForRequest(request, size)
        : std::filesystem::path{};
    if (request.cacheMode == IconCacheMode::PreferCache && !cachePath.empty()) {
        ResolvedIcon cached = LoadPngIconFile(cachePath, L"disk-cache:" + cachePath.filename().wstring());
        if (HasPixels(cached)) {
            LogIconResolve(request, size, L"disk-cache", cached.source, cachePath, false, true);
            return cached;
        }
    }
    if (request.kind == IconSourceKind::Link && LooksLikeUrl(request.link)) {
        const std::filesystem::path urlIcon = UrlIconFile(appDirectory_, request.link, true);
        if (!urlIcon.empty()) {
            ResolvedIcon urlResult = LoadPngIconFile(urlIcon, L"url-icon-file");
            if (HasPixels(urlResult)) {
                bool cacheWritten = false;
                if (cacheEnabled && !cachePath.empty()) {
                    cacheWritten = SavePngIcon(urlResult, cachePath);
                }
                LogIconResolve(request, size, L"url-icon-file", urlResult.source, cachePath, cacheWritten, true);
                return urlResult;
            }
        }
    }
    ResolvedIcon result = ResolveShellItemImage(request, size);
    if (HasPixels(result)) {
        bool cacheWritten = false;
        if (cacheEnabled && !cachePath.empty()) {
            cacheWritten = SavePngIcon(result, cachePath);
        }
        LogIconResolve(
            request,
            size,
            IconRouteFromSource(result.source),
            result.source,
            cachePath,
            cacheWritten,
            true);
        return result;
    }
    std::wstring source;
    HICON icon = ResolveIconHandle(request, source);
    if (!icon && request.allowFallback) {
        icon = ResolveFallbackIcon(request.fallbackKind, request.stockIcon, source);
    }
    result = CaptureIcon(icon, size, 1, source);
    if (icon) {
        DestroyIcon(icon);
    }
    if (HasPixels(result) && cacheEnabled && !cachePath.empty()) {
        const bool cacheWritten = SavePngIcon(result, cachePath);
        LogIconResolve(
            request,
            size,
            IconRouteFromSource(result.source),
            result.source,
            cachePath,
            cacheWritten,
            true);
    } else if (HasPixels(result)) {
        LogIconResolve(
            request,
            size,
            IconRouteFromSource(result.source),
            result.source,
            cachePath,
            false,
            true);
    } else {
        LogIconResolve(request, size, L"failed", result.source, cachePath, false, false);
    }
    return result;
}

std::vector<ResolvedIcon> IconResolverService::ResolveBatch(
    const std::vector<IconRequest>& requests,
    std::stop_token stopToken) const {
    std::vector<ResolvedIcon> result;
    result.reserve(requests.size());
    for (const auto& request : requests) {
        if (stopToken.stop_requested()) {
            break;
        }
        result.push_back(Resolve(request, stopToken));
    }
    return result;
}

ResolvedIcon IconResolverService::ResolveAppxUnplatedIcon(const std::wstring& parseName, int size) const {
    const auto packageFamily = PackageFamilyFromAumid(parseName);
    if (!packageFamily) {
        return {};
    }
    const auto packageRoot = PackageInstallPath(*packageFamily);
    if (!packageRoot) {
        return {};
    }
    const std::filesystem::path manifestPath = *packageRoot / L"AppxManifest.xml";
    const auto manifestLogo = AppxVisualElementLogo(manifestPath);
    if (!manifestLogo || Trim(*manifestLogo).empty()) {
        return {};
    }

    for (const auto& candidate : AppxLogoCandidates(*packageRoot, *manifestLogo, size)) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            continue;
        }
        ResolvedIcon icon = LoadPngIcon(candidate, L"appx-unplated-logo");
        if (HasPixels(icon)) {
            return icon;
        }
    }
    return {};
}

ResolvedIcon IconResolverService::ResolveShellItemImage(const IconRequest& request, int size) const {
    switch (request.kind) {
    case IconSourceKind::Link:
        return ResolveLinkShellItemImage(request.link, size);
    case IconSourceKind::PidlBlob:
        return ResolvePidlImage(request.pidl, size, L"shell-item-image-pidl");
    case IconSourceKind::ShellParseName:
        return ResolveShellParseNameImage(request.value, size, L"shell-item-image-parse-name");
    default:
        return {};
    }
}

ResolvedIcon IconResolverService::ResolveLinkShellItemImage(const Link& link, int size) const {
    if (MenuIconIsRenderable(SystemFunctionMenuIconForLink(link)) || LooksLikeUrl(link)) {
        return {};
    }
    const std::wstring iconPath = Trim(link.icon);
    if (!iconPath.empty() && iconPath != L"#url" && iconPath != L"默认系统缓存图标") {
        return {};
    }
    ResolvedIcon result = ResolveAppxUnplatedIcon(link.path, size);
    if (HasPixels(result)) {
        return result;
    }
    result = ResolvePidlImage(link.pidl, size, L"shell-item-image-link-pidl");
    if (HasPixels(result)) {
        return result;
    }
    return ResolveShellParseNameImage(link.path, size, L"shell-item-image-link-parse-name");
}

ResolvedIcon IconResolverService::ResolvePidlImage(
    const std::vector<std::uint8_t>& pidl,
    int size,
    const std::wstring& source) const {
    if (!ShellItemService::IsPidlBlobPlausible(pidl)) {
        return {};
    }
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromIDList(
            reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl.data()),
            IID_PPV_ARGS(&factory))) || !factory) {
        return {};
    }
    HBITMAP bitmap = nullptr;
    const SIZE requested{size, size};
    const HRESULT hr = factory->GetImage(requested, SIIGBF_ICONONLY, &bitmap);
    SafeRelease(factory);
    if (FAILED(hr) || !bitmap) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return {};
    }
    ResolvedIcon result = CaptureBitmap(bitmap, 2, source);
    DeleteObject(bitmap);
    return result;
}

ResolvedIcon IconResolverService::ResolveShellParseNameImage(
    const std::wstring& value,
    int size,
    const std::wstring& source) const {
    const std::wstring target = ExpandEnvironmentStringsSafe(Trim(value));
    if (target.empty() || !ShellItemService::IsShellParseName(target)) {
        return {};
    }
    ResolvedIcon appxIcon = ResolveAppxUnplatedIcon(target, size);
    if (HasPixels(appxIcon)) {
        appxIcon.source = source + L"-appx-unplated";
        return appxIcon;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(target.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) {
        return {};
    }
    const UINT bytes = ILGetSize(pidl);
    std::vector<std::uint8_t> blob(bytes);
    if (bytes > 0) {
        std::memcpy(blob.data(), pidl, bytes);
    }
    CoTaskMemFree(pidl);
    return ResolvePidlImage(blob, size, source);
}

ResolvedIcon IconResolverService::CaptureBitmap(
    HBITMAP bitmap,
    int quality,
    const std::wstring& source) const {
    BITMAP object{};
    if (!bitmap || GetObjectW(bitmap, sizeof(object), &object) != sizeof(object) ||
        object.bmWidth <= 0 || object.bmHeight == 0) {
        return {};
    }
    const int width = object.bmWidth;
    const int height = std::abs(object.bmHeight);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    ResolvedIcon result;
    result.width = width;
    result.height = height;
    result.quality = quality;
    result.pixels.resize(static_cast<std::size_t>(width) * height);
    HDC dc = CreateCompatibleDC(nullptr);
    const int rows = dc ? GetDIBits(
        dc,
        bitmap,
        0,
        static_cast<UINT>(height),
        result.pixels.data(),
        &info,
        DIB_RGB_COLORS) : 0;
    if (dc) {
        DeleteDC(dc);
    }
    if (rows != height) {
        return {};
    }

    bool hasAlpha = false;
    bool hasColor = false;
    for (const auto pixel : result.pixels) {
        hasAlpha = hasAlpha || (pixel >> 24) != 0;
        hasColor = hasColor || (pixel & 0x00FFFFFFu) != 0;
    }
    if (!hasAlpha && hasColor) {
        for (auto& pixel : result.pixels) {
            if ((pixel & 0x00FFFFFFu) != 0) {
                pixel |= 0xFF000000u;
            }
        }
    } else if (ContainsStraightAlphaPixels(result.pixels)) {
        // The main UI uploads resolved icons as premultiplied BGRA. Some Shell
        // item images arrive as straight-alpha DIBs, which makes translucent
        // folder edges appear too bright unless normalized here.
        PremultiplyTranslucentPixels(result.pixels);
    }
    result.ok = hasColor || hasAlpha;
    result.source = source;
    return result;
}

ResolvedIcon IconResolverService::ResolveContextMenuProvider(
    const TrackedContextMenuProviderBinding& binding,
    int size,
    std::stop_token stopToken) const {
    if (stopToken.stop_requested()) {
        return {};
    }
    if ((binding.providerId ? binding.providerId : L"") != ShellContextMenuProviderId::Terminal) {
        ShellContextMenuItem item;
        TrackedProviderIconSource providerSource = TrackedProviderIconSource::None;
        if (ShellItemService::LoadTrackedProviderIcon(binding, item, &providerSource) &&
            item.iconWidth > 0 && item.iconHeight > 0 &&
            item.iconPixels.size() == static_cast<std::size_t>(item.iconWidth * item.iconHeight)) {
            ResolvedIcon result;
            result.ok = true;
            result.width = item.iconWidth;
            result.height = item.iconHeight;
            result.quality = std::max(1, item.iconQuality);
            result.pixels = item.iconPixels;
            result.source = L"context-menu-provider";
            return result;
        }
        return {};
    }
    std::wstring source;
    HICON icon = ResolveProviderIcon(binding, source);
    ResolvedIcon result = CaptureIcon(icon, std::clamp(size, 1, 256), 1, source);
    if (icon) {
        DestroyIcon(icon);
    }
    return result;
}

HICON IconResolverService::ResolveIconHandle(const IconRequest& request, std::wstring& source) const {
    switch (request.kind) {
    case IconSourceKind::Link:
        return ResolveLinkIcon(request.link, source);
    case IconSourceKind::FilePath:
        return ResolveFileIcon(request.value, false, source);
    case IconSourceKind::DirectoryPath:
        return ResolveFileIcon(request.value, true, source);
    case IconSourceKind::Url:
        return ResolveStockIcon(SIID_WORLD, source);
    case IconSourceKind::ShellParseName:
        return ResolveShellParseNameIcon(request.value, source);
    case IconSourceKind::PidlBlob:
        return ResolvePidlIcon(request.pidl, source);
    case IconSourceKind::IconLocation:
        return ResolveIconLocation(request.value, source);
    case IconSourceKind::CommandLine:
        return ResolveCommandIcon(request, source);
    case IconSourceKind::ContextMenuProvider:
        return ResolveProviderIcon(request.providerId, source);
    case IconSourceKind::Stock:
        return ResolveStockIcon(request.stockIcon, source);
    case IconSourceKind::DefaultCategory:
        return ResolveFallbackIcon(request.fallbackKind, request.stockIcon, source);
    default:
        return nullptr;
    }
}

HICON IconResolverService::ResolveLinkIcon(const Link& link, std::wstring& source) const {
    const MenuIcon menuIcon = SystemFunctionMenuIconForLink(link);
    if (MenuIconIsRenderable(menuIcon)) {
        if (HICON icon = CreateTablerIconHandle(
                appDirectory_,
                MenuIconTablerId(menuIcon),
                64,
                RGB(0, 153, 215))) {
            source = L"tabler-system";
            return icon;
        }
    }

    const std::wstring iconPath = Trim(link.icon);
    if (!iconPath.empty() && iconPath != L"#url" && iconPath != L"默认系统缓存图标") {
        if (HICON icon = ResolveIconLocation(iconPath, source)) {
            return icon;
        }
        if (HICON icon = ResolveFileIcon(iconPath, false, source)) {
            return icon;
        }
    }
    if (LooksLikeUrl(link)) {
        return ResolveStockIcon(SIID_WORLD, source);
    }
    if (HICON icon = ResolvePidlIcon(link.pidl, source)) {
        return icon;
    }
    if (HICON icon = ResolveShellParseNameIcon(link.path, source)) {
        return icon;
    }
    return ResolveFileIcon(link.path, link.type == 1, source);
}

HICON IconResolverService::ResolveProviderIcon(const std::wstring& providerId, std::wstring& source) const {
    if (providerId == ShellContextMenuProviderId::Terminal) {
        const TerminalContextMenuRefreshContext context = TerminalContextMenuService::DetectAvailablePrograms();
        for (const auto& program : context.programs) {
            if (HICON icon = ResolveFileIcon(program.executable, false, source)) {
                source = L"terminal-executable";
                return icon;
            }
        }
        return nullptr;
    }

    const auto provider = FindProvider(providerId);
    if (!provider) {
        return nullptr;
    }
    return ResolveProviderIcon(*provider, source);
}

HICON IconResolverService::ResolveProviderIcon(
    const TrackedContextMenuProviderBinding& provider,
    std::wstring& source) const {
    ShellContextMenuItem item;
    TrackedProviderIconSource providerSource = TrackedProviderIconSource::None;
    if (!ShellItemService::LoadTrackedProviderIcon(provider, item, &providerSource)) {
        return nullptr;
    }
    if (item.iconPixels.empty() || item.iconWidth <= 0 || item.iconHeight <= 0) {
        return nullptr;
    }
    ResolvedIcon resolved;
    resolved.ok = true;
    resolved.width = item.iconWidth;
    resolved.height = item.iconHeight;
    resolved.quality = std::max(1, item.iconQuality);
    resolved.pixels = item.iconPixels;
    resolved.source = L"context-menu-provider";
    source = resolved.source;
    HBITMAP bitmap = CreateBitmapFromPixels(resolved, std::max(item.iconWidth, item.iconHeight), RGB(255, 255, 255));
    if (!bitmap) {
        return nullptr;
    }
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = bitmap;
    iconInfo.hbmMask = bitmap;
    HICON icon = CreateIconIndirect(&iconInfo);
    DeleteObject(bitmap);
    return icon;
}

HICON IconResolverService::ResolveIconLocation(const std::wstring& value, std::wstring& source) const {
    std::wstring iconPath;
    int iconIndex = 0;
    if (!ParseIconLocation(value, iconPath, iconIndex)) {
        return nullptr;
    }
    iconPath = ResolveExecutablePath(iconPath);
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    if (ExtractIconExW(iconPath.c_str(), iconIndex, &largeIcon, &smallIcon, 1) != 0) {
        source = L"icon-location";
        if (largeIcon && smallIcon) {
            DestroyIcon(smallIcon);
            return largeIcon;
        }
        if (largeIcon) {
            return largeIcon;
        }
        if (smallIcon) {
            return smallIcon;
        }
    }
    return ResolveFileIcon(iconPath, false, source);
}

HICON IconResolverService::ResolveCommandIcon(const IconRequest& request, std::wstring& source) const {
    const std::wstring executable = ExecutableFromCommand(request.value);
    if (request.preferFallbackForGenericHost &&
        (IsGenericHostExecutable(executable) || IsScriptLikeTarget(executable))) {
        return nullptr;
    }
    return ResolveFileIcon(executable, false, source);
}

HICON IconResolverService::ResolveFileIcon(const std::wstring& value, bool directory, std::wstring& source) const {
    const std::wstring path = ResolveExecutablePath(value);
    if (Trim(path).empty()) {
        return nullptr;
    }
    SHFILEINFOW info{};
    const DWORD attrs = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
    if (SHGetFileInfoW(path.c_str(), attrs, &info, sizeof(info), flags)) {
        source = directory ? L"directory" : L"file";
        return info.hIcon;
    }
    flags |= SHGFI_USEFILEATTRIBUTES;
    if (SHGetFileInfoW(path.c_str(), attrs, &info, sizeof(info), flags)) {
        source = directory ? L"directory-attributes" : L"file-attributes";
        return info.hIcon;
    }
    return nullptr;
}

HICON IconResolverService::ResolvePidlIcon(const std::vector<std::uint8_t>& pidl, std::wstring& source) const {
    if (!ShellItemService::IsPidlBlobPlausible(pidl)) {
        return nullptr;
    }
    SHFILEINFOW info{};
    if (SHGetFileInfoW(
            reinterpret_cast<LPCWSTR>(pidl.data()),
            0,
            &info,
            sizeof(info),
            SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON)) {
        source = L"pidl";
        return info.hIcon;
    }
    return nullptr;
}

HICON IconResolverService::ResolveShellParseNameIcon(const std::wstring& value, std::wstring& source) const {
    const std::wstring target = ExpandEnvironmentStringsSafe(Trim(value));
    if (target.empty() || !ShellItemService::IsShellParseName(target)) {
        return nullptr;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(target.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) {
        return nullptr;
    }
    SHFILEINFOW info{};
    HICON icon = nullptr;
    if (SHGetFileInfoW(
            reinterpret_cast<LPCWSTR>(pidl),
            0,
            &info,
            sizeof(info),
            SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON)) {
        source = L"shell-parse-name";
        icon = info.hIcon;
    }
    CoTaskMemFree(pidl);
    return icon;
}

HICON IconResolverService::ResolveStockIcon(SHSTOCKICONID iconId, std::wstring& source) const {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(SHGetStockIconInfo(iconId, SHGSI_ICON | SHGSI_LARGEICON, &info))) {
        source = L"stock";
        return info.hIcon;
    }
    source = L"default-application";
    return DuplicateIconHandle(LoadIconW(nullptr, IDI_APPLICATION));
}

HICON IconResolverService::ResolveFallbackIcon(
    IconFallbackKind kind,
    SHSTOCKICONID stockIcon,
    std::wstring& source) const {
    if (HICON icon = CreateTablerIconHandle(appDirectory_, FallbackTablerIcon(kind), 52, FallbackIconColor(kind))) {
        source = L"fallback-" + IconFallbackKindKey(kind);
        return icon;
    }
    HICON icon = ResolveStockIcon(stockIcon, source);
    source = L"fallback-stock-" + IconFallbackKindKey(kind);
    return icon;
}

ResolvedIcon IconResolverService::CaptureIcon(HICON icon, int size, int quality, const std::wstring& source) const {
    if (!icon || size <= 0) {
        return {};
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!bitmap || !dc || !bits) {
        if (dc) {
            DeleteDC(dc);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return {};
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    std::fill_n(static_cast<std::uint32_t*>(bits), static_cast<std::size_t>(size) * size, 0);
    const BOOL drew = DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    GdiFlush();

    ResolvedIcon result;
    if (drew) {
        result.width = size;
        result.height = size;
        result.quality = quality;
        result.pixels.resize(static_cast<std::size_t>(size) * size);
        std::memcpy(result.pixels.data(), bits, result.pixels.size() * sizeof(std::uint32_t));
        result.ok = std::any_of(result.pixels.begin(), result.pixels.end(), [](std::uint32_t pixel) {
            return (pixel >> 24) != 0 || (pixel & 0x00FFFFFFu) != 0;
        });
        PremultiplyTranslucentPixels(result.pixels);
        result.source = source;
    }
    DeleteDC(dc);
    DeleteObject(bitmap);
    return result;
}

HBITMAP IconResolverService::CreateBitmapFromPixels(
    const ResolvedIcon& icon,
    int targetSize,
    COLORREF background,
    bool preserveTransparency) {
    if (!HasPixels(icon) || targetSize <= 0) {
        return nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = targetSize;
    info.bmiHeader.biHeight = -targetSize;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return nullptr;
    }
    auto* target = static_cast<std::uint32_t*>(bits);
    const std::uint32_t bg = preserveTransparency
        ? 0u
        : 0xFF000000u |
            (static_cast<std::uint32_t>(GetRValue(background)) << 16) |
            (static_cast<std::uint32_t>(GetGValue(background)) << 8) |
            static_cast<std::uint32_t>(GetBValue(background));
    std::fill_n(target, static_cast<std::size_t>(targetSize) * targetSize, bg);

    const double scale = std::min(
        static_cast<double>(targetSize) / icon.width,
        static_cast<double>(targetSize) / icon.height);
    const int drawWidth = std::max(1, static_cast<int>(std::round(icon.width * scale)));
    const int drawHeight = std::max(1, static_cast<int>(std::round(icon.height * scale)));
    const int offsetX = (targetSize - drawWidth) / 2;
    const int offsetY = (targetSize - drawHeight) / 2;

    for (int y = 0; y < drawHeight; ++y) {
        const int srcY = std::clamp(static_cast<int>(y / scale), 0, icon.height - 1);
        for (int x = 0; x < drawWidth; ++x) {
            const int srcX = std::clamp(static_cast<int>(x / scale), 0, icon.width - 1);
            const std::uint32_t src = icon.pixels[static_cast<std::size_t>(srcY) * icon.width + srcX];
            const std::uint32_t alpha = src >> 24;
            if (alpha == 0) {
                continue;
            }
            const std::size_t dstIndex = static_cast<std::size_t>(offsetY + y) * targetSize + offsetX + x;
            if (preserveTransparency) {
                target[dstIndex] = src;
                continue;
            }
            if (alpha == 255) {
                target[dstIndex] = src;
                continue;
            }
            const std::uint32_t dst = target[dstIndex];
            const std::uint32_t srcR = (src >> 16) & 0xFFu;
            const std::uint32_t srcG = (src >> 8) & 0xFFu;
            const std::uint32_t srcB = src & 0xFFu;
            const std::uint32_t dstR = (dst >> 16) & 0xFFu;
            const std::uint32_t dstG = (dst >> 8) & 0xFFu;
            const std::uint32_t dstB = dst & 0xFFu;
            target[dstIndex] =
                0xFF000000u |
                (((srcR * alpha + dstR * (255 - alpha)) / 255) << 16) |
                (((srcG * alpha + dstG * (255 - alpha)) / 255) << 8) |
                ((srcB * alpha + dstB * (255 - alpha)) / 255);
        }
    }
    return bitmap;
}
