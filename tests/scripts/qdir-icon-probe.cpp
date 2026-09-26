#include "IconResolverService.h"
#include "ShellItemService.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

// Read-only diagnosis. All icon acquisition goes through the production facade.
// No windows, input injection, application launch, or production cache writes.
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::wstring path = argv[1];
    const std::filesystem::path output = argv[2];
    std::filesystem::create_directories(output);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IconResolverService resolver({}, output / L"isolated-cache");
    std::ofstream report(output / L"resolver-results.tsv");
    report << "case\tsize\tsource\twidth\theight\tquality\topaque_pixels\tsaturated_pixels\tsaved\n";
    int errors = 0;
    std::ofstream roundtrip(output / L"cache-roundtrip.tsv");
    roundtrip << "size\tdimensions_equal\tchanged_pixels\tmax_channel_delta\talpha_changed\n";
    auto capture = [&](std::string name, IconRequest request) {
        const ResolvedIcon icon = resolver.Resolve(request);
        int opaque = 0, saturated = 0;
        for (auto p : icon.pixels) {
            if ((p >> 24) < 32) continue;
            ++opaque;
            int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (std::max({r,g,b}) > 96 && std::max({r,g,b}) - std::min({r,g,b}) > 48)
                ++saturated;
        }
        const bool saved = IconResolverService::SavePngIcon(icon,
            output / (name + "-" + std::to_string(request.size) + ".png"));
        std::string source;
        for (wchar_t c : icon.source) source.push_back(c <= 127 ? static_cast<char>(c) : '?');
        report << name << '\t' << request.size << '\t'
            << source << '\t'
            << icon.width << '\t' << icon.height << '\t' << icon.quality << '\t'
            << opaque << '\t' << saturated << '\t' << saved << '\n';
        if (!saved) ++errors;
        return icon;
    };
    Link link;
    link.path = path;
    link.name = L"Q-Dir";
    link.type = 0;
    const auto item = ShellItemService::FromPathOrParseName(path);
    for (int size : {16,20,24,32,40,48,64,96,128,256}) {
        auto request = IconResolverService::ForLink(link, size);
        request.cacheMode = IconCacheMode::Disabled;
        capture("link", request);
        if (item) {
            request.link.pidl = item->pidl;
            capture("link-pidl", request);
            request.link.pidl.clear();
        }
        request.kind = IconSourceKind::ShellParseName;
        request.value = path;
        capture("shell", request);
        request.kind = IconSourceKind::FilePath;
        capture("file-best", request);
        request.kind = IconSourceKind::IconLocation;
        for (const int id : {0, -128, -204, -329, -330, -332}) {
            request.value = path + L"," + std::to_wstring(id);
            capture("resource" + std::to_string(id), request);
        }
        request = IconResolverService::ForLink(link, size);
        request.cacheMode = IconCacheMode::Refresh;
        const auto fresh = capture("cache-refresh", request);
        request.cacheMode = IconCacheMode::PreferCache;
        const auto cached = capture("cache-read", request);
        const bool dimensionsEqual = fresh.width == cached.width && fresh.height == cached.height &&
            fresh.pixels.size() == cached.pixels.size();
        std::size_t changed = 0, alphaChanged = 0;
        int maxDelta = 0;
        if (dimensionsEqual) {
            for (std::size_t i = 0; i < fresh.pixels.size(); ++i) {
                const auto a = fresh.pixels[i], b = cached.pixels[i];
                changed += a != b;
                alphaChanged += (a >> 24) != (b >> 24);
                for (int shift : {0, 8, 16, 24})
                    maxDelta = std::max(maxDelta, std::abs(int((a >> shift) & 255) - int((b >> shift) & 255)));
            }
        } else ++errors;
        roundtrip << size << '\t' << dimensionsEqual << '\t' << changed << '\t' << maxDelta << '\t' << alphaChanged << '\n';
        // Preserve raw service output to distinguish alpha conversion from icon identity.
        for (const auto* icon : {&fresh, &cached}) {
            const auto name = icon == &fresh ? "cache-refresh" : "cache-read";
            std::ofstream raw(output / (std::string(name) + "-" + std::to_string(size) + ".bgra"), std::ios::binary);
            raw.write(reinterpret_cast<const char*>(icon->pixels.data()), icon->pixels.size() * sizeof(std::uint32_t));
        }
    }
    // Resource IDs from the local PE's RT_GROUP_ICON table, not production rules.
    for (int id : {128,129,130,204,205,215,222,226,233,249,250,306,311,312,
                   314,315,317,318,320,321,323,325,329,330,332,341,345,346,350,362}) {
        IconRequest request;
        request.kind = IconSourceKind::IconLocation;
        request.size = 32;
        request.value = path + L",-" + std::to_wstring(id);
        request.cacheMode = IconCacheMode::Disabled;
        capture("resource-" + std::to_string(id), request);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    std::cout << "qdir_icon_probe_errors=" << errors << '\n';
    return errors ? 1 : 0;
}
