#include "ShellContextMenuCacheService.h"

#include "Utilities.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>

namespace {
constexpr std::array<char, 8> kMagic{{'Q', 'S', 'C', 'M', 'E', 'N', 'U', '6'}};
constexpr std::uint32_t kMaxEntries = 10000;
constexpr std::uint32_t kMaxItems = 1000;
constexpr std::uint32_t kMaxIcons = 10000;
constexpr std::uint32_t kMaxTextLength = 32768;

template <typename T>
bool ReadValue(std::istream& stream, T& value) {
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename T>
void WriteValue(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool ReadString(std::istream& stream, std::wstring& value) {
    std::uint32_t length = 0;
    if (!ReadValue(stream, length) || length > kMaxTextLength) {
        return false;
    }
    value.resize(length);
    if (length == 0) {
        return true;
    }
    return static_cast<bool>(stream.read(
        reinterpret_cast<char*>(value.data()),
        static_cast<std::streamsize>(length * sizeof(wchar_t))));
}

void WriteString(std::ostream& stream, const std::wstring& value) {
    const auto length = static_cast<std::uint32_t>(std::min<std::size_t>(value.size(), kMaxTextLength));
    WriteValue(stream, length);
    if (length != 0) {
        stream.write(
            reinterpret_cast<const char*>(value.data()),
            static_cast<std::streamsize>(length * sizeof(wchar_t)));
    }
}

bool ReadItem(std::istream& stream, ShellContextMenuItem& item, std::uint32_t depth) {
    if (depth > 3) {
        return false;
    }
    std::uint8_t flags = 0;
    std::uint8_t actionKind = 0;
    std::uint32_t childCount = 0;
    if (!ReadString(stream, item.providerId) || !ReadValue(stream, flags) ||
        !ReadString(stream, item.text) || !ReadString(stream, item.verb) ||
        !ReadValue(stream, actionKind) || actionKind > static_cast<std::uint8_t>(ShellContextMenuActionKind::Terminal) ||
        !ReadString(stream, item.actionId) || !ReadString(stream, item.executable) ||
        !ReadString(stream, item.arguments) || !ReadString(stream, item.workingDirectory) ||
        !ReadValue(stream, childCount) || childCount > kMaxItems) {
        return false;
    }
    item.actionKind = static_cast<ShellContextMenuActionKind>(actionKind);
    item.enabled = (flags & 0x01) != 0;
    item.checked = (flags & 0x02) != 0;
    item.separator = (flags & 0x04) != 0;
    item.children.resize(childCount);
    for (auto& child : item.children) {
        if (!ReadItem(stream, child, depth + 1)) {
            return false;
        }
    }
    return true;
}

void WriteItem(std::ostream& stream, const ShellContextMenuItem& item, std::uint32_t depth) {
    const std::uint8_t flags = static_cast<std::uint8_t>(
        (item.enabled ? 0x01 : 0) |
        (item.checked ? 0x02 : 0) |
        (item.separator ? 0x04 : 0));
    WriteString(stream, item.providerId);
    WriteValue(stream, flags);
    WriteString(stream, item.text);
    WriteString(stream, item.verb);
    WriteValue(stream, static_cast<std::uint8_t>(item.actionKind));
    WriteString(stream, item.actionId);
    WriteString(stream, item.executable);
    WriteString(stream, item.arguments);
    WriteString(stream, item.workingDirectory);
    const auto childCount = depth < 3
        ? static_cast<std::uint32_t>(std::min<std::size_t>(item.children.size(), kMaxItems))
        : 0;
    WriteValue(stream, childCount);
    for (std::uint32_t index = 0; index < childCount; ++index) {
        WriteItem(stream, item.children[index], depth + 1);
    }
}

bool ReadIcon(std::istream& stream, ShellContextMenuCachedIcon& icon) {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t quality = 0;
    std::uint32_t pixelCount = 0;
    if (!ReadValue(stream, width) || !ReadValue(stream, height) ||
        !ReadValue(stream, quality) || !ReadValue(stream, pixelCount) ||
        width <= 0 || height <= 0 || width > 64 || height > 64 ||
        pixelCount != static_cast<std::uint32_t>(width * height)) {
        return false;
    }
    icon.width = width;
    icon.height = height;
    icon.quality = quality;
    icon.pixels.resize(pixelCount);
    return static_cast<bool>(stream.read(
        reinterpret_cast<char*>(icon.pixels.data()),
        static_cast<std::streamsize>(pixelCount * sizeof(std::uint32_t))));
}

void WriteIcon(std::ostream& stream, const ShellContextMenuCachedIcon& icon) {
    const std::int32_t width = icon.width;
    const std::int32_t height = icon.height;
    const std::int32_t quality = icon.quality;
    const std::uint32_t pixelCount = static_cast<std::uint32_t>(icon.pixels.size());
    WriteValue(stream, width);
    WriteValue(stream, height);
    WriteValue(stream, quality);
    WriteValue(stream, pixelCount);
    stream.write(
        reinterpret_cast<const char*>(icon.pixels.data()),
        static_cast<std::streamsize>(pixelCount * sizeof(std::uint32_t)));
}

bool MatchesProvider(const ShellContextMenuItem& item, const std::wstring& providerId) {
    return item.providerId == providerId;
}
}

ShellContextMenuCacheService::ShellContextMenuCacheService(std::filesystem::path appDirectory)
    : cachePath_(std::move(appDirectory) / L"cache" / L"shell-context-menu.bin") {
    Load();
}

std::wstring ShellContextMenuCacheService::TargetKey(const Link& link) {
    return ToLower(ExpandEnvironmentStringsSafe(Trim(link.path)));
}

std::wstring ShellContextMenuCacheService::IconKey(
    const ShellContextMenuItem& item,
    const std::vector<std::wstring>& path) {
    if (item.separator || item.providerId.empty()) {
        return {};
    }
    const std::wstring provider = ToLower(item.providerId);
    if (item.actionKind == ShellContextMenuActionKind::Terminal && !item.actionId.empty()) {
        return provider + L"|terminal|" + ToLower(item.actionId) + L"|" +
            ToLower(std::filesystem::path(item.executable).filename().wstring());
    }
    const std::wstring verb = ToLower(Trim(item.verb));
    if (!verb.empty()) {
        return provider + L"|verb|" + verb;
    }
    const auto normalizeText = [](const std::wstring& source) {
        std::wstring result;
        result.reserve(source.size());
        bool quoted = false;
        bool placeholderWritten = false;
        for (wchar_t ch : ToLower(Trim(source))) {
            if (ch == L'"') {
                if (!quoted && !placeholderWritten) {
                    result += L"{value}";
                    placeholderWritten = true;
                }
                quoted = !quoted;
                continue;
            }
            if (ch == L'“') {
                if (!quoted && !placeholderWritten) {
                    result += L"{value}";
                    placeholderWritten = true;
                }
                quoted = true;
                continue;
            }
            if (ch == L'”') {
                quoted = false;
                continue;
            }
            if (!quoted) {
                result.push_back(ch);
            }
        }
        return Trim(result);
    };
    std::wstring key = provider + L"|path";
    for (const auto& segment : path) {
        key += L"|" + normalizeText(segment);
    }
    return key;
}

void ShellContextMenuCacheService::IngestIcons(
    std::vector<ShellContextMenuItem>& items,
    std::vector<std::wstring>& path) {
    for (auto& item : items) {
        if (item.separator) {
            continue;
        }
        path.push_back(item.text);
        const bool validIcon = item.iconWidth > 0 && item.iconHeight > 0 &&
            item.iconWidth <= 64 && item.iconHeight <= 64 &&
            item.iconPixels.size() == static_cast<std::size_t>(item.iconWidth * item.iconHeight);
        if (validIcon) {
            const std::wstring key = IconKey(item, path);
            if (!key.empty()) {
                ShellContextMenuCachedIcon candidate;
                candidate.width = item.iconWidth;
                candidate.height = item.iconHeight;
                candidate.quality = std::max(1, item.iconQuality);
                candidate.pixels = item.iconPixels;
                const auto found = iconPool_.find(key);
                const int candidateArea = candidate.width * candidate.height;
                const int existingArea = found == iconPool_.end() ? 0 : found->second.width * found->second.height;
                if (found == iconPool_.end() ||
                    candidate.quality > found->second.quality ||
                    (candidate.quality == found->second.quality && candidateArea > existingArea)) {
                    iconPool_[key] = std::move(candidate);
                }
            }
        }
        IngestIcons(item.children, path);
        path.pop_back();
    }
}

void ShellContextMenuCacheService::ApplyIcons(
    std::vector<ShellContextMenuItem>& items,
    std::vector<std::wstring>& path) const {
    for (auto& item : items) {
        if (item.separator) {
            continue;
        }
        path.push_back(item.text);
        const auto found = iconPool_.find(IconKey(item, path));
        if (found != iconPool_.end()) {
            item.iconWidth = found->second.width;
            item.iconHeight = found->second.height;
            item.iconQuality = found->second.quality;
            item.iconPixels = found->second.pixels;
        }
        ApplyIcons(item.children, path);
        path.pop_back();
    }
}

void ShellContextMenuCacheService::StripIcons(std::vector<ShellContextMenuItem>& items) {
    for (auto& item : items) {
        item.iconWidth = 0;
        item.iconHeight = 0;
        item.iconQuality = 0;
        item.iconPixels.clear();
        StripIcons(item.children);
    }
}

std::vector<ShellContextMenuItem> ShellContextMenuCacheService::ItemsFor(
    const Link& link,
    const ShellContextMenuTrackingOptions& tracking) const {
    const auto found = entries_.find(link.id);
    if (found == entries_.end() || found->second.target != TargetKey(link)) {
        return {};
    }
    std::vector<ShellContextMenuItem> result;
    for (const auto& item : found->second.items) {
        if (tracking.Includes(item.providerId)) {
            result.push_back(item);
        }
    }
    std::vector<std::wstring> path;
    ApplyIcons(result, path);
    return result;
}

void ShellContextMenuCacheService::Update(
    const Link& link,
    const ShellContextMenuSnapshot& snapshot,
    const ShellContextMenuTrackingOptions& tracking) {
    if (!snapshot.complete) {
        return;
    }
    std::vector<ShellContextMenuItem> snapshotItems = snapshot.items;
    std::vector<std::wstring> path;
    IngestIcons(snapshotItems, path);
    StripIcons(snapshotItems);

    Entry& entry = entries_[link.id];
    entry.target = TargetKey(link);
    entry.items.erase(std::remove_if(entry.items.begin(), entry.items.end(), [&](const auto& item) {
        return tracking.Includes(item.providerId);
    }), entry.items.end());
    for (const auto& item : snapshotItems) {
        if (tracking.Includes(item.providerId)) {
            entry.items.push_back(item);
        }
    }
    if (entry.items.empty()) {
        entries_.erase(link.id);
    }
    Save();
}

bool ShellContextMenuCacheService::Reset() {
    entries_.clear();
    iconPool_.clear();
    return Save();
}

void ShellContextMenuCacheService::Remove(int linkId) {
    if (entries_.erase(linkId) != 0) {
        Save();
    }
}

void ShellContextMenuCacheService::RemoveProvider(const std::wstring& providerId) {
    bool changed = false;
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        auto& items = entry->second.items;
        const auto oldSize = items.size();
        items.erase(std::remove_if(items.begin(), items.end(), [&](const auto& item) {
            return MatchesProvider(item, providerId);
        }), items.end());
        changed = changed || oldSize != items.size();
        if (items.empty()) {
            entry = entries_.erase(entry);
        } else {
            ++entry;
        }
    }
    const std::wstring iconPrefix = ToLower(providerId) + L"|";
    for (auto icon = iconPool_.begin(); icon != iconPool_.end();) {
        if (icon->first.rfind(iconPrefix, 0) == 0) {
            icon = iconPool_.erase(icon);
            changed = true;
        } else {
            ++icon;
        }
    }
    if (changed) {
        Save();
    }
}

void ShellContextMenuCacheService::Load() {
    std::ifstream stream(cachePath_, std::ios::binary);
    if (!stream) {
        return;
    }
    std::array<char, 8> magic{};
    std::uint32_t entryCount = 0;
    if (!stream.read(magic.data(), magic.size()) || magic != kMagic ||
        !ReadValue(stream, entryCount) || entryCount > kMaxEntries) {
        return;
    }
    std::unordered_map<int, Entry> loaded;
    for (std::uint32_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
        std::int32_t linkId = 0;
        Entry entry;
        std::uint32_t itemCount = 0;
        if (!ReadValue(stream, linkId) || linkId <= 0 || !ReadString(stream, entry.target) ||
            !ReadValue(stream, itemCount) || itemCount > kMaxItems) {
            return;
        }
        entry.items.resize(itemCount);
        for (auto& item : entry.items) {
            if (!ReadItem(stream, item, 0)) {
                return;
            }
        }
        loaded.emplace(linkId, std::move(entry));
    }
    std::uint32_t iconCount = 0;
    if (!ReadValue(stream, iconCount) || iconCount > kMaxIcons) {
        return;
    }
    std::unordered_map<std::wstring, ShellContextMenuCachedIcon> loadedIcons;
    for (std::uint32_t iconIndex = 0; iconIndex < iconCount; ++iconIndex) {
        std::wstring key;
        ShellContextMenuCachedIcon icon;
        if (!ReadString(stream, key) || key.empty() || !ReadIcon(stream, icon)) {
            return;
        }
        loadedIcons.emplace(std::move(key), std::move(icon));
    }
    entries_ = std::move(loaded);
    iconPool_ = std::move(loadedIcons);
}

bool ShellContextMenuCacheService::Save() const {
    std::error_code ec;
    std::filesystem::create_directories(cachePath_.parent_path(), ec);
    if (ec) {
        return false;
    }
    // Write to a sibling temp file and atomically replace the cache so a crash
    // or partial write can never leave the on-disk cache truncated (which Load()
    // rejects wholesale, discarding every cached menu and icon).
    std::filesystem::path tempPath = cachePath_;
    tempPath += L".tmp";
    {
        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return false;
        }
        stream.write(kMagic.data(), kMagic.size());
        const auto entryCount = static_cast<std::uint32_t>(std::min<std::size_t>(entries_.size(), kMaxEntries));
        WriteValue(stream, entryCount);
        std::uint32_t written = 0;
        for (const auto& [linkId, entry] : entries_) {
            if (written++ >= entryCount) {
                break;
            }
            const auto storedLinkId = static_cast<std::int32_t>(linkId);
            WriteValue(stream, storedLinkId);
            WriteString(stream, entry.target);
            const auto itemCount = static_cast<std::uint32_t>(std::min<std::size_t>(entry.items.size(), kMaxItems));
            WriteValue(stream, itemCount);
            for (std::uint32_t index = 0; index < itemCount; ++index) {
                WriteItem(stream, entry.items[index], 0);
            }
        }
        const auto iconCount = static_cast<std::uint32_t>(std::min<std::size_t>(iconPool_.size(), kMaxIcons));
        WriteValue(stream, iconCount);
        written = 0;
        for (const auto& [key, icon] : iconPool_) {
            if (written++ >= iconCount) {
                break;
            }
            WriteString(stream, key);
            WriteIcon(stream, icon);
        }
        stream.flush();
        if (!stream.good()) {
            stream.close();
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }
    std::filesystem::rename(tempPath, cachePath_, ec);
    if (ec) {
        // rename can fail across some filesystems or if the destination is
        // locked; fall back to an overwriting copy and clean up the temp file.
        ec.clear();
        std::filesystem::copy_file(
            tempPath, cachePath_, std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code removeEc;
        std::filesystem::remove(tempPath, removeEc);
        if (ec) {
            return false;
        }
    }
    return true;
}
