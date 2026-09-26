#pragma once

#include "Models.h"

#include <memory>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

struct LinkSearchResult {
    int linkId = 0;
    std::wstring groupName;
    std::wstring tagName;
    bool allTermsInName = false;
};

std::vector<std::wstring> ParseLinkSearchTerms(const std::wstring& query);

// Immutable search index built from a read-only AppModel snapshot. Name/path
// normalization and business ordering are paid once when the dialog opens;
// individual queries only scan compact entries and return stable IDs.
class LinkSearchIndex final {
public:
    static std::shared_ptr<const LinkSearchIndex> Build(
        const AppModel& model,
        std::stop_token stopToken = {});

    // Returns nullptr when cancellation was requested. Otherwise the returned
    // vector is the complete ordered result snapshot and may safely be shared
    // between a background query and UI paging.
    std::shared_ptr<const std::vector<int>> SearchIds(
        const std::wstring& query,
        std::stop_token stopToken = {}) const;

    const LinkSearchResult* FindResult(int linkId) const;
    std::size_t size() const noexcept;

private:
    struct Entry {
        int linkId = 0;
        std::wstring normalizedName;
        std::wstring normalizedPath;
        LinkSearchResult result;
    };

    std::vector<Entry> entries_;
    std::unordered_map<int, std::size_t> resultIndexById_;
};

std::vector<LinkSearchResult> SearchLinks(const AppModel& model, const std::wstring& query);
