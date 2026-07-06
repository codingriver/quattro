#ifdef QUATTRO_WITH_HTTPLIB
#include <winsock2.h>
#include <ws2tcpip.h>
#include <httplib.h>
#endif

#include "../src/AppLog.h"
#include "../src/LocalHttpServerService.h"
#include "../src/Utilities.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
bool Require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        return false;
    }
    return true;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
}

bool WriteWideUtf8File(const std::filesystem::path& path, const std::wstring& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), size, nullptr, nullptr);
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::wstring RuntimeConfigText(
    bool authEnabled,
    bool guestUpload = true,
    bool guestWrite = true,
    bool allowPost = true,
    bool allowPut = true,
    int maxUploadBytes = 1048576) {
    return
        L"[http]\r\n"
        L"GuestRead=" + std::wstring(authEnabled ? L"0" : L"1") + L"\r\n"
        L"GuestUpload=" + std::wstring(guestUpload ? L"1" : L"0") + L"\r\n"
        L"GuestWrite=" + std::wstring(guestWrite ? L"1" : L"0") + L"\r\n"
        L"AuthEnabled=" + std::wstring(authEnabled ? L"1" : L"0") + L"\r\n"
        L"Username=tester\r\n"
        L"Password=secret\r\n"
        L"DirectoryListing=1\r\n"
        L"ShowHiddenFiles=0\r\n"
        L"AllowPost=" + std::wstring(allowPost ? L"1" : L"0") + L"\r\n"
        L"AllowPut=" + std::wstring(allowPut ? L"1" : L"0") + L"\r\n"
        L"AllowRemoteOpen=0\r\n"
        L"DefaultDisposition=attachment\r\n"
        L"OpenExtensions=.txt .html\r\n"
        L"DownloadExtensions=.bin .zip\r\n"
        L"MaxUploadBytes=" + std::to_wstring(maxUploadBytes) + L"\r\n"
        L"MimeMap=.txt=text/plain; charset=utf-8|.html=text/html; charset=utf-8|.bin=application/octet-stream\r\n";
}

#ifdef QUATTRO_WITH_HTTPLIB
bool WaitForHealth(int port) {
    for (int i = 0; i < 80; ++i) {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(0, 200000);
        client.set_read_timeout(0, 200000);
        if (auto response = client.Get("/_quattro/health")) {
            if (response->status == 200) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

bool StartOnAvailablePort(LocalHttpServerService& service, LocalHttpServerOptions& options, std::wstring& error) {
    const int basePort = 43127 + static_cast<int>(GetCurrentProcessId() % 1000);
    for (int offset = 0; offset < 80; ++offset) {
        options.port = basePort + offset;
        if (service.Start(options, error)) {
            if (WaitForHealth(options.port)) {
                return true;
            }
            service.Stop();
        }
    }
    return false;
}

SOCKET OccupyLocalPort(int port) {
    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port));
    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(socketHandle, SOMAXCONN) != 0) {
        closesocket(socketHandle);
        return INVALID_SOCKET;
    }
    return socketHandle;
}

httplib::Client MakeClient(int port) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(5, 0);
    return client;
}
#endif
}

int wmain() {
#ifndef QUATTRO_WITH_HTTPLIB
    std::wcout << L"HTTP runtime acceptance skipped: QUATTRO_WITH_HTTPLIB is not enabled.\n";
    return 0;
#else
    const auto root = std::filesystem::temp_directory_path() / (L"quattro-http-runtime-" + std::to_wstring(GetCurrentProcessId()));
    const auto userConfig = std::filesystem::temp_directory_path() / (L"quattro-http-runtime-user-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", userConfig.c_str());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(userConfig, ec);
    std::filesystem::create_directories(root / L"nested", ec);
    InitializeAppLog(root);

    bool ok = true;
    ok = Require(WriteTextFile(root / L"a.txt", "0123456789abcdef"), L"seed text file should be written") && ok;
    ok = Require(WriteTextFile(root / L"download.bin", "bin-body"), L"seed binary file should be written") && ok;
    ok = Require(WriteTextFile(root / L"nested" / L"child.txt", "nested"), L"nested file should be written") && ok;
    ok = Require(WriteTextFile(root / L"index.html", "<html>home-index</html>"), L"index file should be written") && ok;
    ok = Require(WriteTextFile(root / L"hidden.txt", "hidden-body"), L"hidden file should be written") && ok;
    SetFileAttributesW((root / L"hidden.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);
    const std::string largeBody(2 * 1024 * 1024, 'L');
    ok = Require(WriteTextFile(root / L"large.bin", largeBody), L"large file should be written") && ok;
    std::wstring error;
    ok = Require(LocalHttpServerService::EnsureDetailConfig(root, error), L"default detail config should be created") && ok;
    ok = Require(WriteWideUtf8File(LocalHttpServerService::DetailConfigPath(root), RuntimeConfigText(false)), L"detail config should be writable") && ok;

    LocalHttpServerService service;
    LocalHttpServerOptions options;
    options.rootPath = root;
    options.lanAccess = false;
    ok = Require(StartOnAvailablePort(service, options, error), L"HTTP server should start on an available local port") && ok;

    httplib::Client client = MakeClient(options.port);

    auto health = client.Get("/_quattro/health");
    ok = Require(health && health->status == 200 && health->body.find("\"ok\":true") != std::string::npos, L"health endpoint should return ok") && ok;

    auto listing = client.Get("/");
    ok = Require(listing && listing->status == 200, L"directory listing should be served") && ok;
    ok = Require(listing && listing->body.find("home-index") != std::string::npos && listing->body.find("Index of /") == std::string::npos, L"index.html should take priority over directory listing") && ok;

    auto nestedListing = client.Get("/nested/");
    ok = Require(nestedListing && nestedListing->status == 200, L"nested directory listing should be served") && ok;
    ok = Require(nestedListing && nestedListing->body.find("Index of /nested/") != std::string::npos && nestedListing->body.find("child.txt") != std::string::npos, L"directory listing should include nested files") && ok;

    auto hidden = client.Get("/hidden.txt");
    ok = Require(hidden && hidden->status == 403, L"hidden files should be forbidden by default") && ok;

    auto file = client.Get("/a.txt");
    ok = Require(file && file->status == 200 && file->body == "0123456789abcdef", L"GET should serve file body") && ok;
    ok = Require(file && file->get_header_value("Content-Type").find("text/plain") != std::string::npos, L"GET should use configured MIME") && ok;
    ok = Require(file && file->get_header_value("Content-Disposition").find("inline") != std::string::npos, L"text files should open inline") && ok;

    httplib::Headers rangeHeaders{{"Range", "bytes=2-5"}};
    auto range = client.Get("/a.txt", rangeHeaders);
    ok = Require(range && range->status == 206, L"Range GET should return 206") && ok;
    ok = Require(range && range->body == "2345", L"Range GET should return partial body") && ok;
    ok = Require(range && range->get_header_value("Content-Range") == "bytes 2-5/16", L"Range GET should return Content-Range") && ok;

    auto download = client.Get("/download.bin");
    ok = Require(download && download->status == 200, L"binary download should be served") && ok;
    ok = Require(download && download->get_header_value("Content-Disposition").find("attachment") != std::string::npos, L"download extensions should use attachment disposition") && ok;

    auto large = client.Get("/large.bin");
    ok = Require(large && large->status == 200 && large->body.size() == largeBody.size(), L"large download should return full body") && ok;

    auto blockedTraversal = client.Get("/%2e%2e/outside.txt");
    ok = Require(blockedTraversal && blockedTraversal->status == 403, L"path traversal should be forbidden") && ok;

    auto put = client.Put("/put/created.txt", "put-body", "text/plain");
    ok = Require(put && put->status == 201, L"PUT should create a file") && ok;
    ok = Require(LoadUtf8File(root / L"put" / L"created.txt") == L"put-body", L"PUT-created file should contain request body") && ok;

    httplib::UploadFormDataItems uploadItems{
        {"file", "upload-body", "posted.txt", "text/plain"},
        {"file", "second-body", "second.txt", "text/plain"},
    };
    auto post = client.Post("/_quattro/upload?path=/uploads/", uploadItems);
    ok = Require(post && post->status == 200, L"POST multipart upload should succeed") && ok;
    ok = Require(LoadUtf8File(root / L"uploads" / L"posted.txt") == L"upload-body", L"POST-uploaded file should be written") && ok;
    ok = Require(LoadUtf8File(root / L"uploads" / L"second.txt") == L"second-body", L"POST multi-file upload should write all files") && ok;

    std::vector<std::thread> workers;
    std::vector<bool> workerOk(8, false);
    for (int i = 0; i < static_cast<int>(workerOk.size()); ++i) {
        workers.emplace_back([&, i]() {
            httplib::Client worker = MakeClient(options.port);
            auto response = worker.Get("/a.txt");
            workerOk[static_cast<std::size_t>(i)] = response && response->status == 200 && response->body == "0123456789abcdef";
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    ok = Require(std::all_of(workerOk.begin(), workerOk.end(), [](bool value) { return value; }), L"concurrent GET requests should succeed") && ok;

    service.Stop();
    SOCKET occupiedSocket = OccupyLocalPort(options.port);
    ok = Require(occupiedSocket != INVALID_SOCKET, L"test should occupy local HTTP port") && ok;
    if (occupiedSocket != INVALID_SOCKET) {
        std::wstring conflictError;
        ok = Require(!service.Start(options, conflictError), L"server should fail when port is already occupied") && ok;
        closesocket(occupiedSocket);
    }
    ok = Require(service.Start(options, error), L"server should restart after occupied port is released") && ok;
    ok = Require(WaitForHealth(options.port), L"server should respond after port conflict test") && ok;

    service.Stop();
    options.lanAccess = true;
    ok = Require(service.Start(options, error), L"LAN-enabled server should bind successfully") && ok;
    ok = Require(WaitForHealth(options.port), L"LAN-enabled server should still respond locally") && ok;
    ok = Require(service.BaseUrl(false).find(L"localhost") != std::wstring::npos, L"LAN-enabled base URL should use shareable host label") && ok;

    ok = Require(WriteWideUtf8File(LocalHttpServerService::DetailConfigPath(root), RuntimeConfigText(false, true, true, true, true, 4)), L"small upload limit config should be writable") && ok;
    ok = Require(service.Restart(options, error), L"server should restart for upload limit config") && ok;
    ok = Require(WaitForHealth(options.port), L"server should respond after upload limit restart") && ok;
    httplib::Client limitClient = MakeClient(options.port);
    httplib::UploadFormDataItems tooLargeItems{{"file", "too-large", "too-large.txt", "text/plain"}};
    auto tooLarge = limitClient.Post("/_quattro/upload?path=/limited/", tooLargeItems);
    ok = Require(tooLarge && tooLarge->status == 413, L"MaxUploadBytes should reject oversized POST uploads") && ok;

    ok = Require(WriteWideUtf8File(LocalHttpServerService::DetailConfigPath(root), RuntimeConfigText(false, true, true, false, true)), L"AllowPost disabled config should be writable") && ok;
    ok = Require(service.Restart(options, error), L"server should restart for AllowPost config") && ok;
    ok = Require(WaitForHealth(options.port), L"server should respond after AllowPost restart") && ok;
    httplib::Client allowPostClient = MakeClient(options.port);
    auto postDisabled = allowPostClient.Post("/_quattro/upload?path=/blocked-post/", uploadItems);
    ok = Require(postDisabled && postDisabled->status == 403, L"AllowPost=0 should reject POST uploads") && ok;

    ok = Require(WriteWideUtf8File(LocalHttpServerService::DetailConfigPath(root), RuntimeConfigText(false, true, true, true, false)), L"AllowPut disabled config should be writable") && ok;
    ok = Require(service.Restart(options, error), L"server should restart for AllowPut config") && ok;
    ok = Require(WaitForHealth(options.port), L"server should respond after AllowPut restart") && ok;
    httplib::Client allowPutClient = MakeClient(options.port);
    auto putDisabled = allowPutClient.Put("/blocked-put.txt", "body", "text/plain");
    ok = Require(putDisabled && putDisabled->status == 403, L"AllowPut=0 should reject PUT writes") && ok;

    ok = Require(WriteWideUtf8File(LocalHttpServerService::DetailConfigPath(root), RuntimeConfigText(false, false, true, true, true)), L"GuestUpload disabled config should be writable") && ok;
    ok = Require(service.Restart(options, error), L"server should restart for GuestUpload config") && ok;
    ok = Require(WaitForHealth(options.port), L"server should respond after GuestUpload restart") && ok;
    httplib::Client guestUploadClient = MakeClient(options.port);
    auto guestUploadDisabled = guestUploadClient.Post("/_quattro/upload?path=/guest-upload/", uploadItems);
    ok = Require(guestUploadDisabled && guestUploadDisabled->status == 403, L"GuestUpload=0 should reject guest POST uploads") && ok;

    ok = Require(WriteWideUtf8File(LocalHttpServerService::DetailConfigPath(root), RuntimeConfigText(false, true, false, true, true)), L"GuestWrite disabled config should be writable") && ok;
    ok = Require(service.Restart(options, error), L"server should restart for GuestWrite config") && ok;
    ok = Require(WaitForHealth(options.port), L"server should respond after GuestWrite restart") && ok;
    httplib::Client guestWriteClient = MakeClient(options.port);
    auto guestWriteDisabled = guestWriteClient.Put("/guest-write.txt", "body", "text/plain");
    ok = Require(guestWriteDisabled && guestWriteDisabled->status == 403, L"GuestWrite=0 should reject guest PUT writes") && ok;

    httplib::Client openClient = MakeClient(options.port);
    auto openMissingUrl = openClient.Get("/_quattro/open");
    ok = Require(openMissingUrl && openMissingUrl->status == 400, L"open endpoint should reject missing url without ShellExecute") && ok;

    ok = Require(WriteWideUtf8File(LocalHttpServerService::DetailConfigPath(root), RuntimeConfigText(true, false, false)), L"auth detail config should be writable") && ok;
    ok = Require(service.Restart(options, error), L"server should restart and reload detail config") && ok;
    ok = Require(WaitForHealth(options.port), L"server should respond after restart") && ok;

    httplib::Client unauthenticated("127.0.0.1", options.port);
    auto unauthorized = unauthenticated.Get("/a.txt");
    ok = Require(unauthorized && unauthorized->status == 401, L"auth-enabled server should reject guest reads") && ok;

    httplib::Client authenticated("127.0.0.1", options.port);
    authenticated.set_basic_auth("tester", "secret");
    auto authorized = authenticated.Get("/a.txt");
    ok = Require(authorized && authorized->status == 200 && authorized->body == "0123456789abcdef", L"valid Basic Auth should allow reads") && ok;

    service.Stop();
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(userConfig, ec);
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", nullptr);
    if (!ok) {
        return 1;
    }
    std::wcout << L"HTTP server runtime acceptance passed.\n";
    return 0;
#endif
}
