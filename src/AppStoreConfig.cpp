#include "AppStoreConfig.h"

#include "Utilities.h"

namespace {
std::wstring StripPrefix(std::wstring value, const std::wstring& prefix) {
    if (ToLower(value).rfind(ToLower(prefix), 0) == 0) {
        value.erase(0, prefix.size());
    }
    return value;
}
}

std::wstring NormalizeAppStoreRepository(std::wstring repository) {
    repository = Trim(repository);
    repository = ReplaceAll(repository, L"\\", L"/");
    repository = StripPrefix(repository, L"https://github.com/");
    repository = StripPrefix(repository, L"http://github.com/");
    repository = StripPrefix(repository, L"git@github.com:");
    repository = StripPrefix(repository, L"github.com/");
    while (!repository.empty() && repository.front() == L'/') {
        repository.erase(repository.begin());
    }
    while (!repository.empty() && repository.back() == L'/') {
        repository.pop_back();
    }
    if (ToLower(repository).size() > 4 && ToLower(repository).substr(ToLower(repository).size() - 4) == L".git") {
        repository.resize(repository.size() - 4);
    }
    return repository;
}

std::wstring AppStoreRepositoryFor(const std::wstring& owner, const std::wstring& repo) {
    const std::wstring ownerText = Trim(owner);
    const std::wstring repoText = Trim(repo);
    if (ownerText.empty() || repoText.empty()) {
        return {};
    }
    return NormalizeAppStoreRepository(ownerText + L"/" + repoText);
}

std::wstring AppStoreRepositoryOwner(const std::wstring& repository) {
    const std::wstring value = NormalizeAppStoreRepository(repository);
    const std::size_t slash = value.find(L'/');
    return slash == std::wstring::npos ? L"" : value.substr(0, slash);
}

std::wstring AppStoreRepositoryName(const std::wstring& repository) {
    const std::wstring value = NormalizeAppStoreRepository(repository);
    const std::size_t slash = value.find(L'/');
    if (slash == std::wstring::npos) {
        return {};
    }
    const std::size_t nextSlash = value.find(L'/', slash + 1);
    return value.substr(slash + 1, nextSlash == std::wstring::npos ? std::wstring::npos : nextSlash - slash - 1);
}

std::wstring AppStoreRepositoryOwner(const AppConfig& config) {
    const std::wstring owner = AppStoreRepositoryOwner(config.appStoreRepository);
    if (!owner.empty()) {
        return owner;
    }
    return Trim(config.appStoreOwner);
}

std::wstring AppStoreRepositoryName(const AppConfig& config) {
    const std::wstring repo = AppStoreRepositoryName(config.appStoreRepository);
    if (!repo.empty()) {
        return repo;
    }
    return Trim(config.appStoreRepo);
}
