#pragma once

#include "Models.h"

#include <string>

std::wstring NormalizeAppStoreRepository(std::wstring repository);
std::wstring AppStoreRepositoryFor(const std::wstring& owner, const std::wstring& repo);
std::wstring AppStoreRepositoryOwner(const std::wstring& repository);
std::wstring AppStoreRepositoryName(const std::wstring& repository);
std::wstring AppStoreRepositoryOwner(const AppConfig& config);
std::wstring AppStoreRepositoryName(const AppConfig& config);
