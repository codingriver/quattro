#include "AppPackageService.h"

#include "JsonValue.h"
#include "Utilities.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace {
constexpr std::uint64_t kDefaultSplitBytes = 256ull * 1024ull * 1024ull;
constexpr int kPbkdf2Iterations = 210000;

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring output(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length);
    return output;
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring output;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': output += L"\\\\"; break;
        case L'"': output += L"\\\""; break;
        case L'\n': output += L"\\n"; break;
        case L'\r': output += L"\\r"; break;
        case L'\t': output += L"\\t"; break;
        default: output.push_back(ch); break;
        }
    }
    return output;
}

bool WriteUtf8File(const std::filesystem::path& path, const std::wstring& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    const std::string utf8 = WideToUtf8(text);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return file.good();
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool WriteFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::wstring BytesToHex(const std::vector<std::uint8_t>& bytes) {
    std::wstringstream stream;
    stream << std::hex << std::setfill(L'0');
    for (std::uint8_t byte : bytes) {
        stream << std::setw(2) << static_cast<int>(byte);
    }
    return stream.str();
}

std::wstring Sha256Bytes(const std::vector<std::uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    DWORD objectLength = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &copied, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<std::uint8_t> object(objectLength);
    std::vector<std::uint8_t> hash(32);
    BCRYPT_HASH_HANDLE handle = nullptr;
    if (BCryptCreateHash(algorithm, &handle, object.data(), objectLength, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    PUCHAR data = const_cast<PUCHAR>(bytes.empty() ? reinterpret_cast<const UCHAR*>("") : bytes.data());
    const bool ok = BCryptHashData(handle, data, static_cast<ULONG>(bytes.size()), 0) == 0 &&
                    BCryptFinishHash(handle, hash.data(), static_cast<ULONG>(hash.size()), 0) == 0;
    BCryptDestroyHash(handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? BytesToHex(hash) : L"";
}

bool RandomBytes(std::vector<std::uint8_t>& bytes) {
    return BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

bool DeriveKey(const std::wstring& token, const std::vector<std::uint8_t>& salt, std::vector<std::uint8_t>& key) {
    key.assign(32, 0);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return false;
    }
    const std::string password = WideToUtf8(token);
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()),
        kPbkdf2Iterations,
        key.data(),
        static_cast<ULONG>(key.size()),
        0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0;
}

bool AesGcmTransform(
    bool encrypt,
    const std::vector<std::uint8_t>& input,
    const std::vector<std::uint8_t>& key,
    const std::vector<std::uint8_t>& nonce,
    std::vector<std::uint8_t>& output,
    std::vector<std::uint8_t>& tag) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    const ULONG chainingModeBytes = static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t));
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), chainingModeBytes, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    if (BCryptGenerateSymmetricKey(algorithm, &keyHandle, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    output.assign(input.size(), 0);
    if (encrypt) {
        tag.assign(16, 0);
    }
    std::vector<std::uint8_t> macContext(16, 0);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = const_cast<PUCHAR>(nonce.data());
    authInfo.cbNonce = static_cast<ULONG>(nonce.size());
    authInfo.pbTag = tag.data();
    authInfo.cbTag = static_cast<ULONG>(tag.size());
    authInfo.pbMacContext = macContext.data();
    authInfo.cbMacContext = static_cast<ULONG>(macContext.size());

    ULONG result = 0;
    const NTSTATUS status = encrypt
        ? BCryptEncrypt(
              keyHandle,
              const_cast<PUCHAR>(input.empty() ? reinterpret_cast<const UCHAR*>("") : input.data()),
              static_cast<ULONG>(input.size()),
              &authInfo,
              nullptr,
              0,
              output.empty() ? nullptr : output.data(),
              static_cast<ULONG>(output.size()),
              &result,
              0)
        : BCryptDecrypt(
              keyHandle,
              const_cast<PUCHAR>(input.empty() ? reinterpret_cast<const UCHAR*>("") : input.data()),
              static_cast<ULONG>(input.size()),
              &authInfo,
              nullptr,
              0,
              output.empty() ? nullptr : output.data(),
              static_cast<ULONG>(output.size()),
              &result,
              0);
    BCryptDestroyKey(keyHandle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0;
}

std::wstring StringValue(const JsonValue& value, const wchar_t* key, const std::wstring& fallback = L"") {
    const JsonValue* child = value.get(key);
    return child ? child->stringOr(fallback) : fallback;
}

bool BoolValue(const JsonValue& value, const wchar_t* key, bool fallback = false) {
    const JsonValue* child = value.get(key);
    return child ? child->boolOr(fallback) : fallback;
}

bool LoadUploadMetadata(const std::filesystem::path& path, AppStoreManifest& manifest, std::wstring& error) {
    const std::wstring text = LoadUtf8File(path);
    if (text.empty()) {
        error = L"应用目录缺少 manifest.json。";
        return false;
    }
    JsonValue root;
    if (!ParseJson(text, root, error)) {
        return false;
    }
    if (!root.isObject()) {
        error = L"manifest.json 不是对象。";
        return false;
    }
    manifest.schema = 1;
    manifest.appId = StringValue(root, L"appId");
    manifest.name = StringValue(root, L"name");
    manifest.displayName = StringValue(root, L"displayName", manifest.name);
    manifest.version = StringValue(root, L"version");
    manifest.tag = StringValue(root, L"tag");
    manifest.category = ToLower(StringValue(root, L"category", L"app"));
    manifest.sourceKind = ToLower(StringValue(root, L"sourceKind"));
    manifest.originalName = StringValue(root, L"originalName");
    manifest.summary = StringValue(root, L"summary");
    manifest.description = StringValue(root, L"description");
    manifest.author = StringValue(root, L"author");
    manifest.homepage = StringValue(root, L"homepage");
    manifest.license = StringValue(root, L"license");
    manifest.draft = BoolValue(root, L"draft", false);
    if (manifest.appId.empty() || manifest.name.empty() || manifest.version.empty()) {
        error = L"manifest.json 至少需要 appId、name、version。";
        return false;
    }
    if (manifest.category.empty()) {
        manifest.category = L"app";
    }
    if (manifest.category != L"app" && manifest.category != L"other") {
        error = L"manifest.json category 只支持 app 或 other。";
        return false;
    }
    if (manifest.displayName.empty()) {
        manifest.displayName = manifest.name;
    }
    if (manifest.tag.empty()) {
        manifest.tag = AppStoreTagFor(L"{appId}-v{version}", manifest.appId, manifest.version);
    }
    return true;
}

std::wstring ManifestJson(const AppStoreManifest& manifest) {
    std::wstringstream out;
    out << L"{\n";
    out << L"  \"schema\": 1,\n";
    out << L"  \"appId\": \"" << JsonEscape(manifest.appId) << L"\",\n";
    out << L"  \"name\": \"" << JsonEscape(manifest.name) << L"\",\n";
    out << L"  \"displayName\": \"" << JsonEscape(manifest.displayName.empty() ? manifest.name : manifest.displayName) << L"\",\n";
    out << L"  \"version\": \"" << JsonEscape(manifest.version) << L"\",\n";
    out << L"  \"tag\": \"" << JsonEscape(manifest.tag) << L"\",\n";
    out << L"  \"category\": \"" << JsonEscape(manifest.category.empty() ? L"app" : manifest.category) << L"\",\n";
    out << L"  \"sourceKind\": \"" << JsonEscape(manifest.sourceKind) << L"\",\n";
    out << L"  \"originalName\": \"" << JsonEscape(manifest.originalName) << L"\",\n";
    out << L"  \"summary\": \"" << JsonEscape(manifest.summary) << L"\",\n";
    out << L"  \"description\": \"" << JsonEscape(manifest.description) << L"\",\n";
    out << L"  \"author\": \"" << JsonEscape(manifest.author) << L"\",\n";
    out << L"  \"homepage\": \"" << JsonEscape(manifest.homepage) << L"\",\n";
    out << L"  \"license\": \"" << JsonEscape(manifest.license) << L"\",\n";
    out << L"  \"draft\": " << (manifest.draft ? L"true" : L"false") << L",\n";
    out << L"  \"package\": {\n";
    out << L"    \"format\": \"zip\",\n";
    out << L"    \"encrypted\": " << (manifest.encrypted ? L"true" : L"false") << L",\n";
    out << L"    \"splitSize\": " << manifest.splitSize << L",\n";
    out << L"    \"totalSize\": " << manifest.totalSize << L",\n";
    out << L"    \"sha256\": \"" << JsonEscape(manifest.packageSha256) << L"\",\n";
    out << L"    \"plainSha256\": \"" << JsonEscape(manifest.plainSha256) << L"\",\n";
    out << L"    \"parts\": [\n";
    for (std::size_t i = 0; i < manifest.parts.size(); ++i) {
        const auto& part = manifest.parts[i];
        out << L"      {\"index\": " << part.index << L", \"name\": \"" << JsonEscape(part.name) << L"\", \"size\": " << part.size << L", \"sha256\": \"" << JsonEscape(part.sha256) << L"\"}";
        out << (i + 1 < manifest.parts.size() ? L"," : L"") << L"\n";
    }
    out << L"    ]\n";
    out << L"  },\n";
    out << L"  \"encryption\": {\n";
    out << L"    \"enabled\": " << (manifest.encryptionEnabled ? L"true" : L"false") << L",\n";
    out << L"    \"algorithm\": \"" << JsonEscape(manifest.encryptionAlgorithm) << L"\",\n";
    out << L"    \"kdf\": \"" << JsonEscape(manifest.encryptionKdf) << L"\",\n";
    out << L"    \"iterations\": " << manifest.encryptionIterations << L",\n";
    out << L"    \"salt\": \"" << JsonEscape(manifest.encryptionSalt) << L"\",\n";
    out << L"    \"nonce\": \"" << JsonEscape(manifest.encryptionNonce) << L"\",\n";
    out << L"    \"tag\": \"" << JsonEscape(manifest.encryptionTag) << L"\"\n";
    out << L"  }\n";
    out << L"}\n";
    return out.str();
}

bool RunPowershellZip(const std::filesystem::path& source, const std::filesystem::path& target, std::wstring& error) {
    const std::wstring script =
        L"$ErrorActionPreference='Stop';"
        L"Add-Type -AssemblyName System.IO.Compression.FileSystem;"
        L"$src='" + ReplaceAll(source.wstring(), L"'", L"''") + L"';"
        L"$dst='" + ReplaceAll(target.wstring(), L"'", L"''") + L"';"
        L"if(Test-Path -LiteralPath $dst){Remove-Item -LiteralPath $dst -Force};"
        L"[System.IO.Compression.ZipFile]::CreateFromDirectory($src,$dst,[System.IO.Compression.CompressionLevel]::Optimal,$false,[System.Text.Encoding]::UTF8);";
    std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command " + QuoteForCommandLine(script);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        error = L"启动 zip 压缩失败: " + FormatLastError(GetLastError());
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0 || !FileExists(target)) {
        error = L"zip 压缩失败。";
        return false;
    }
    return true;
}

bool SplitPackage(const std::filesystem::path& packagePath, const std::filesystem::path& outDir, const std::wstring& baseName, std::uint64_t splitSize, AppStoreManifest& manifest, std::vector<std::filesystem::path>& partPaths, std::wstring& error) {
    std::ifstream input(packagePath, std::ios::binary);
    if (!input) {
        error = L"无法读取待分片包: " + packagePath.wstring();
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(splitSize, 8ull * 1024ull * 1024ull)));
    int index = 1;
    while (input) {
        std::wstringstream suffix;
        suffix << std::setw(3) << std::setfill(L'0') << index;
        const std::wstring partName = baseName + L".part" + suffix.str();
        const std::filesystem::path partPath = outDir / partName;
        std::ofstream output(partPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = L"无法创建分片: " + partPath.wstring();
            return false;
        }
        std::uint64_t written = 0;
        while (written < splitSize && input) {
            const std::uint64_t wanted = std::min<std::uint64_t>(buffer.size(), splitSize - written);
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(wanted));
            const std::streamsize got = input.gcount();
            if (got <= 0) {
                break;
            }
            output.write(reinterpret_cast<const char*>(buffer.data()), got);
            written += static_cast<std::uint64_t>(got);
        }
        output.close();
        if (written == 0) {
            std::filesystem::remove(partPath, ec);
            break;
        }
        AppPackagePart part;
        part.index = index;
        part.name = partName;
        part.size = written;
        part.sha256 = AppStoreSha256File(partPath);
        if (part.sha256.empty()) {
            error = L"计算分片 SHA-256 失败。";
            return false;
        }
        manifest.parts.push_back(part);
        partPaths.push_back(partPath);
        ++index;
    }
    return !manifest.parts.empty();
}
}

std::wstring AppStoreBase64Encode(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }
    DWORD needed = 0;
    if (!CryptBinaryToStringW(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &needed)) {
        return {};
    }
    std::wstring output(needed, L'\0');
    if (!CryptBinaryToStringW(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, output.data(), &needed)) {
        return {};
    }
    output.resize(needed);
    if (!output.empty() && output.back() == L'\0') {
        output.pop_back();
    }
    return output;
}

std::vector<std::uint8_t> AppStoreBase64Decode(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    DWORD needed = 0;
    if (!CryptStringToBinaryW(text.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &needed, nullptr, nullptr)) {
        return {};
    }
    std::vector<std::uint8_t> output(needed);
    if (!CryptStringToBinaryW(text.c_str(), 0, CRYPT_STRING_BASE64, output.data(), &needed, nullptr, nullptr)) {
        return {};
    }
    output.resize(needed);
    return output;
}

std::wstring AppStoreSha256File(const std::filesystem::path& path) {
    return Sha256Bytes(ReadFileBytes(path));
}

std::wstring SerializeAppStoreManifest(const AppStoreManifest& manifest) {
    return ManifestJson(manifest);
}

AppPackageService::AppPackageService(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {
}

AppPackageBuildResult AppPackageService::BuildUploadPackage(
    const std::filesystem::path& sourceDirectory,
    const AppConfig& config,
    const std::wstring& encryptionToken) {
    AppPackageBuildResult result;
    lastError_.clear();
    if (!DirectoryExists(sourceDirectory)) {
        result.message = L"请选择有效的应用目录。";
        return result;
    }
    AppStoreManifest manifest;
    std::wstring error;
    if (!LoadUploadMetadata(sourceDirectory / L"manifest.json", manifest, error)) {
        result.message = error;
        return result;
    }
    manifest.tag = AppStoreTagFor(config.appStoreTagPattern, manifest.appId, manifest.version);
    manifest.splitSize = static_cast<std::uint64_t>(std::max(16, config.appStoreSplitSizeMiB)) * 1024ull * 1024ull;
    if (manifest.splitSize == 0) {
        manifest.splitSize = kDefaultSplitBytes;
    }

    const std::filesystem::path workRoot = appDirectory_ / L"app-store" / L"packages" / manifest.appId / manifest.version;
    std::error_code ec;
    std::filesystem::remove_all(workRoot, ec);
    std::filesystem::create_directories(workRoot, ec);
    const std::filesystem::path zipPath = workRoot / L"package.zip";
    if (!RunPowershellZip(sourceDirectory, zipPath, error)) {
        result.message = error;
        return result;
    }
    manifest.plainSha256 = AppStoreSha256File(zipPath);
    if (manifest.plainSha256.empty()) {
        result.message = L"计算 zip SHA-256 失败。";
        return result;
    }

    std::filesystem::path finalPackage = zipPath;
    std::wstring partBase = L"package.zip";
    if (!Trim(encryptionToken).empty()) {
        std::vector<std::uint8_t> plain = ReadFileBytes(zipPath);
        std::vector<std::uint8_t> salt(16);
        std::vector<std::uint8_t> nonce(12);
        if (!RandomBytes(salt) || !RandomBytes(nonce)) {
            result.message = L"生成加密随机数失败。";
            return result;
        }
        std::vector<std::uint8_t> key;
        std::vector<std::uint8_t> encrypted;
        std::vector<std::uint8_t> tag;
        if (!DeriveKey(encryptionToken, salt, key) || !AesGcmTransform(true, plain, key, nonce, encrypted, tag)) {
            result.message = L"应用包加密失败。";
            return result;
        }
        finalPackage = workRoot / L"package.zip.enc";
        if (!WriteFileBytes(finalPackage, encrypted)) {
            result.message = L"写入加密应用包失败。";
            return result;
        }
        manifest.encrypted = true;
        manifest.encryptionEnabled = true;
        manifest.encryptionAlgorithm = L"AES-256-GCM";
        manifest.encryptionKdf = L"PBKDF2-HMAC-SHA256";
        manifest.encryptionIterations = kPbkdf2Iterations;
        manifest.encryptionSalt = AppStoreBase64Encode(salt);
        manifest.encryptionNonce = AppStoreBase64Encode(nonce);
        manifest.encryptionTag = AppStoreBase64Encode(tag);
        partBase = L"package.zip.enc";
    } else {
        manifest.encrypted = false;
        manifest.encryptionEnabled = false;
        manifest.encryptionIterations = 0;
    }

    manifest.packageSha256 = AppStoreSha256File(finalPackage);
    manifest.totalSize = std::filesystem::file_size(finalPackage, ec);
    if (manifest.packageSha256.empty() || ec) {
        result.message = L"计算应用包摘要失败。";
        return result;
    }

    const std::filesystem::path partsDir = workRoot / L"parts";
    if (!SplitPackage(finalPackage, partsDir, partBase, manifest.splitSize, manifest, result.partPaths, error)) {
        result.message = error.empty() ? L"应用包分片失败。" : error;
        return result;
    }

    result.manifestPath = workRoot / L"manifest.json";
    manifest.manifestJson = ManifestJson(manifest);
    if (!WriteUtf8File(result.manifestPath, manifest.manifestJson)) {
        result.message = L"写入 manifest.json 失败。";
        return result;
    }
    result.ok = true;
    result.message = L"应用包已生成。";
    result.manifest = std::move(manifest);
    result.packagePath = finalPackage;
    return result;
}

bool AppPackageService::AssembleParts(const AppStoreManifest& manifest, const std::filesystem::path& partsDirectory, const std::filesystem::path& outputPath, std::wstring& error) {
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(outputPath.parent_path(), ec);
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = L"无法创建合并包: " + outputPath.wstring();
        return false;
    }
    for (const auto& part : manifest.parts) {
        const std::filesystem::path partPath = partsDirectory / part.name;
        if (!VerifyFileSha256(partPath, part.sha256, error)) {
            return false;
        }
        std::ifstream input(partPath, std::ios::binary);
        output << input.rdbuf();
    }
    output.close();
    return VerifyFileSha256(outputPath, manifest.packageSha256, error);
}

bool AppPackageService::DecryptPackage(const AppStoreManifest& manifest, const std::filesystem::path& encryptedPath, const std::filesystem::path& plainPath, const std::wstring& encryptionToken, std::wstring& error) {
    error.clear();
    if (!manifest.encrypted) {
        std::error_code ec;
        std::filesystem::copy_file(encryptedPath, plainPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = L"复制未加密包失败。";
            return false;
        }
        return true;
    }
    const std::vector<std::uint8_t> encrypted = ReadFileBytes(encryptedPath);
    const std::vector<std::uint8_t> salt = AppStoreBase64Decode(manifest.encryptionSalt);
    const std::vector<std::uint8_t> nonce = AppStoreBase64Decode(manifest.encryptionNonce);
    std::vector<std::uint8_t> tag = AppStoreBase64Decode(manifest.encryptionTag);
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> plain;
    if (!DeriveKey(encryptionToken, salt, key) || !AesGcmTransform(false, encrypted, key, nonce, plain, tag)) {
        error = L"应用包解密失败。";
        return false;
    }
    if (!WriteFileBytes(plainPath, plain)) {
        error = L"写入解密包失败。";
        return false;
    }
    return VerifyFileSha256(plainPath, manifest.plainSha256, error);
}

bool AppPackageService::VerifyFileSha256(const std::filesystem::path& path, const std::wstring& expected, std::wstring& error) {
    const std::wstring actual = ToLower(AppStoreSha256File(path));
    if (actual.empty() || actual != ToLower(expected)) {
        error = L"文件 SHA-256 校验失败: " + path.wstring();
        return false;
    }
    return true;
}
