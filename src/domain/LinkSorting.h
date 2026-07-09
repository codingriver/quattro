#pragma once

#include "Models.h"

#include <string>

std::wstring InitialSortKey(const std::wstring& value);
int DefaultLinkSortDirection(int sort);
bool LinkSortLess(const Link& left, const Link& right, int sortMode, int sortDirection);
bool LinkSortLess(const Link* left, const Link* right, int sortMode, int sortDirection);
