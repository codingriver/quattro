#include "EmbeddedExecutableManager.h"

#include "Utilities.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~ScopedHandle() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return handle_; }
    HANDLE release() { HANDLE value = handle_; handle_ = nullptr; return value; }

private:
    HANDLE handle_ = nullptr;
};

class BCryptAlgorithm {
public:
    ~BCryptAlgorithm() { if (handle_) BCryptCloseAlgorithmProvider(handle_, 0); }
    bool Open() { return BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0; }
    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class BCryptHash {
public:
    ~BCryptHash() { if (handle_) BCryptDestroyHash(handle_); }
    bool Create(BCRYPT_ALG_HANDLE algorithm, std::vector<unsigned char>& object) {
        return BCryptCreateHash(algorithm, &handle_, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0) >= 0;
    }
    BCRYPT_HASH_HANDLE get() const { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

std::wstring LastErrorText(DWORD error = GetLastError()) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : L"Windows error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

bool SafePathComponent(const wchar_t* value) {
    if (!value || !*value) return false;
    const std::wstring component(value);
    return component != L"." && component != L".." &&
        component.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos;
}

std::wstring Hex(const unsigned char* data, std::size_t size) {
    std::wostringstream output;
    output << std::hex << std::setfill(L'0');
    for (std::size_t index = 0; index < size; ++index) output << std::setw(2) << static_cast<unsigned int>(data[index]);
    return output.str();
}

bool Sha256File(const std::filesystem::path& path, std::wstring& hash) {
    BCryptAlgorithm algorithm;
    if (!algorithm.Open()) return false;
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD bytes = 0;
    if (BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &bytes, 0) < 0 ||
        BCryptGetProperty(algorithm.get(), BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &bytes, 0) < 0) {
        return false;
    }
    std::vector<unsigned char> object(objectSize);
    std::vector<unsigned char> digest(hashSize);
    BCryptHash state;
    if (!state.Create(algorithm.get(), object)) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::array<char, 64 * 1024> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count > 0 && BCryptHashData(state.get(), reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) return false;
    }
    if (!file.eof() || BCryptFinishHash(state.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    hash = Hex(digest.data(), digest.size());
    return true;
}

bool SameEmbeddedExecutable(const std::filesystem::path& path, const EmbeddedExecutableDescriptor& descriptor) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    if (std::filesystem::file_size(path, error) != descriptor.size || error) return false;
    std::wstring hash;
    return Sha256File(path, hash) && descriptor.sha256 && _wcsicmp(hash.c_str(), descriptor.sha256) == 0;
}

std::uint64_t StableHash(const std::wstring& value) {
    std::uint64_t hash = 1469598103934665603ull;
    for (wchar_t ch : value) {
        hash ^= static_cast<std::uint16_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring MutexName(const EmbeddedExecutableDescriptor& descriptor) {
    std::wostringstream name;
    name << L"Local\\Quattro.EmbeddedExecutable." << std::hex
         << StableHash(std::wstring(descriptor.id) + L"|" + descriptor.version);
    return name.str();
}

bool WriteAll(HANDLE file, const unsigned char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, data + offset, chunk, &written, nullptr) || written != chunk) return false;
        offset += written;
    }
    return true;
}
}

std::filesystem::path QuattroEmbeddedExecutableRootDirectory() {
    return QuattroUserConfigDirectory() / L"tools";
}

EmbeddedExecutablePrepareResult PrepareEmbeddedExecutable(
    const EmbeddedExecutableDescriptor& descriptor,
    const EmbeddedExecutablePrepareOptions& options) {
    EmbeddedExecutablePrepareResult result;
    if (!SafePathComponent(descriptor.id) || !SafePathComponent(descriptor.fileName) ||
        !SafePathComponent(descriptor.version) || !descriptor.sha256 || !descriptor.data || descriptor.size == 0) {
        result.message = L"内嵌可执行组件描述无效。";
        return result;
    }
    if (options.rootDirectory.empty()) {
        result.message = L"内嵌可执行组件释放目录为空。";
        return result;
    }

    result.path = options.rootDirectory / descriptor.id / descriptor.version / descriptor.fileName;
    if (SameEmbeddedExecutable(result.path, descriptor)) {
        result.success = true;
        return result;
    }

    const std::wstring mutexName = MutexName(descriptor);
    ScopedHandle mutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    if (!mutex.get()) {
        result.message = L"创建内嵌组件释放锁失败：" + LastErrorText();
        return result;
    }
    const DWORD wait = WaitForSingleObject(mutex.get(), 30000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        result.message = L"等待内嵌组件释放超时。";
        return result;
    }

    if (SameEmbeddedExecutable(result.path, descriptor)) {
        ReleaseMutex(mutex.get());
        result.success = true;
        return result;
    }

    std::error_code error;
    const bool existed = std::filesystem::exists(result.path, error) && !error;
    std::filesystem::create_directories(result.path.parent_path(), error);
    if (error) {
        ReleaseMutex(mutex.get());
        result.message = L"创建内嵌组件目录失败：" + result.path.parent_path().wstring();
        return result;
    }

    const std::filesystem::path temporary = result.path.wstring() + L".tmp." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    ScopedHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        ReleaseMutex(mutex.get());
        result.message = L"创建内嵌组件临时文件失败：" + LastErrorText();
        return result;
    }
    if (!WriteAll(file.get(), descriptor.data, descriptor.size) || !FlushFileBuffers(file.get())) {
        const std::wstring writeError = LastErrorText();
        CloseHandle(file.release());
        std::filesystem::remove(temporary, error);
        ReleaseMutex(mutex.get());
        result.message = L"写入内嵌组件失败：" + writeError;
        return result;
    }
    CloseHandle(file.release());

    if (!SameEmbeddedExecutable(temporary, descriptor)) {
        std::filesystem::remove(temporary, error);
        ReleaseMutex(mutex.get());
        result.message = L"内嵌组件临时文件校验失败。";
        return result;
    }
    if (!MoveFileExW(temporary.c_str(), result.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::wstring moveError = LastErrorText();
        std::filesystem::remove(temporary, error);
        ReleaseMutex(mutex.get());
        result.message = L"安装内嵌组件失败：" + moveError;
        return result;
    }
    if (!SameEmbeddedExecutable(result.path, descriptor)) {
        ReleaseMutex(mutex.get());
        result.message = L"内嵌组件最终校验失败。";
        return result;
    }

    ReleaseMutex(mutex.get());
    result.success = true;
    result.written = !existed;
    result.updated = existed;
    return result;
}
