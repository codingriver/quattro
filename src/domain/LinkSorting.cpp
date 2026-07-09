#include "LinkSorting.h"

#include "Utilities.h"

#include <cwctype>

std::wstring InitialSortKey(const std::wstring& value) {
    const std::wstring text = Trim(value);
    if (text.empty()) {
        return L"#";
    }
    const wchar_t first = text.front();
    if ((first >= L'a' && first <= L'z') || (first >= L'A' && first <= L'Z')) {
        std::wstring key(1, static_cast<wchar_t>(std::towupper(first)));
        return key + L"|" + ToLower(text);
    }
    if (first >= L'0' && first <= L'9') {
        return L"0|" + text;
    }
    if (first >= 0x4E00 && first <= 0x9FFF) {
        return L"Z|" + text;
    }
    return L"#|" + text;
}

int DefaultLinkSortDirection(int sort) {
    return sort == 1 ? 1 : 0;
}

bool LinkSortLess(const Link& left, const Link& right, int sortMode, int sortDirection) {
    if (sortMode == 1 && left.runCount != right.runCount) {
        return sortDirection == 0 ? left.runCount < right.runCount : left.runCount > right.runCount;
    }
    if (sortMode == 2) {
        const std::wstring leftName = InitialSortKey(left.name);
        const std::wstring rightName = InitialSortKey(right.name);
        if (leftName != rightName) {
            return sortDirection == 0 ? leftName < rightName : leftName > rightName;
        }
    }
    if (left.pos != right.pos) {
        return left.pos < right.pos;
    }
    return left.id < right.id;
}

bool LinkSortLess(const Link* left, const Link* right, int sortMode, int sortDirection) {
    if (!left || !right) {
        return left != nullptr && right == nullptr;
    }
    return LinkSortLess(*left, *right, sortMode, sortDirection);
}
