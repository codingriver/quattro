#include "LinkSearch.h"

#include "Utilities.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {
struct GroupOrder {
    int pos = std::numeric_limits<int>::max();
    std::size_t index = std::numeric_limits<std::size_t>::max();
};

struct RankedResult {
    LinkSearchResult result;
    std::wstring normalizedName;
    std::wstring normalizedPath;
    GroupOrder group;
    GroupOrder tag;
    int linkPos = std::numeric_limits<int>::max();
    std::size_t linkIndex = std::numeric_limits<std::size_t>::max();
};
}

std::vector<std::wstring> ParseLinkSearchTerms(const std::wstring& query) {
    const std::wstring normalized = ToLower(Trim(query));
    std::vector<std::wstring> terms;
    std::size_t start = 0;
    while (start < normalized.size()) {
        while (start < normalized.size() && std::iswspace(normalized[start])) {
            ++start;
        }
        if (start >= normalized.size()) break;
        std::size_t end = start;
        while (end < normalized.size() && !std::iswspace(normalized[end])) {
            ++end;
        }
        terms.emplace_back(normalized.substr(start, end - start));
        start = end;
    }
    return terms;
}

std::vector<LinkSearchResult> SearchLinks(const AppModel& model, const std::wstring& query) {
    const auto index = LinkSearchIndex::Build(model);
    if (!index) return {};
    const auto ids = index->SearchIds(query);
    if (!ids) return {};

    const std::vector<std::wstring> terms = ParseLinkSearchTerms(query);
    std::unordered_map<int, std::wstring> normalizedNames;
    normalizedNames.reserve(model.links.size());
    for (const Link& link : model.links) {
        normalizedNames.try_emplace(link.id, ToLower(link.name));
    }
    std::vector<LinkSearchResult> results;
    results.reserve(ids->size());
    for (int id : *ids) {
        const LinkSearchResult* indexed = index->FindResult(id);
        if (!indexed) continue;
        LinkSearchResult result = *indexed;
        if (!terms.empty()) {
            const auto name = normalizedNames.find(id);
            result.allTermsInName = name != normalizedNames.end() &&
                std::all_of(terms.begin(), terms.end(), [&](const std::wstring& term) {
                    return name->second.find(term) != std::wstring::npos;
                });
        }
        results.push_back(std::move(result));
    }
    return results;
}

std::shared_ptr<const LinkSearchIndex> LinkSearchIndex::Build(
    const AppModel& model,
    std::stop_token stopToken) {
    std::unordered_map<int, std::size_t> groupIndices;
    groupIndices.reserve(model.groups.size());
    for (std::size_t index = 0; index < model.groups.size(); ++index) {
        if (stopToken.stop_requested()) return nullptr;
        groupIndices.emplace(model.groups[index].id, index);
    }

    std::vector<RankedResult> ranked;
    ranked.reserve(model.links.size());
    std::unordered_set<int> seenLinkIds;
    seenLinkIds.reserve(model.links.size());
    for (std::size_t linkIndex = 0; linkIndex < model.links.size(); ++linkIndex) {
        if (stopToken.stop_requested()) return nullptr;
        const Link& link = model.links[linkIndex];
        if (!seenLinkIds.emplace(link.id).second) continue;

        RankedResult item;
        item.result.linkId = link.id;
        item.normalizedName = ToLower(link.name);
        item.normalizedPath = ToLower(link.path);
        item.linkPos = link.pos;
        item.linkIndex = linkIndex;

        const auto tagIndexIt = groupIndices.find(link.parentGroup);
        if (tagIndexIt != groupIndices.end()) {
            const Group& tag = model.groups[tagIndexIt->second];
            item.result.tagName = tag.name;
            item.tag = GroupOrder{tag.pos, tagIndexIt->second};
            const auto groupIndexIt = groupIndices.find(tag.parentGroup);
            if (groupIndexIt != groupIndices.end()) {
                const Group& group = model.groups[groupIndexIt->second];
                item.result.groupName = group.name;
                item.group = GroupOrder{group.pos, groupIndexIt->second};
            }
        }
        ranked.push_back(std::move(item));
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const RankedResult& left, const RankedResult& right) {
        if (left.group.pos != right.group.pos) return left.group.pos < right.group.pos;
        if (left.group.index != right.group.index) return left.group.index < right.group.index;
        if (left.tag.pos != right.tag.pos) return left.tag.pos < right.tag.pos;
        if (left.tag.index != right.tag.index) return left.tag.index < right.tag.index;
        if (left.linkPos != right.linkPos) return left.linkPos < right.linkPos;
        if (left.linkIndex != right.linkIndex) return left.linkIndex < right.linkIndex;
        return left.result.linkId < right.result.linkId;
    });

    auto index = std::shared_ptr<LinkSearchIndex>(new LinkSearchIndex());
    index->entries_.reserve(ranked.size());
    index->resultIndexById_.reserve(ranked.size());
    for (auto& item : ranked) {
        if (stopToken.stop_requested()) return nullptr;
        Entry entry{};
        entry.linkId = item.result.linkId;
        entry.normalizedName = std::move(item.normalizedName);
        entry.normalizedPath = std::move(item.normalizedPath);
        entry.result = std::move(item.result);
        index->resultIndexById_.emplace(entry.linkId, index->entries_.size());
        index->entries_.push_back(std::move(entry));
    }
    return index;
}

std::shared_ptr<const std::vector<int>> LinkSearchIndex::SearchIds(
    const std::wstring& query,
    std::stop_token stopToken) const {
    const std::vector<std::wstring> terms = ParseLinkSearchTerms(query);
    auto ids = std::make_shared<std::vector<int>>();
    ids->reserve(entries_.size());
    if (terms.empty()) {
        for (const Entry& entry : entries_) {
            if (stopToken.stop_requested()) return nullptr;
            ids->push_back(entry.linkId);
        }
        return ids;
    }

    std::vector<int> secondary;
    secondary.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        if (stopToken.stop_requested()) return nullptr;
        bool matches = true;
        bool allTermsInName = true;
        for (const std::wstring& term : terms) {
            const bool nameMatch = entry.normalizedName.find(term) != std::wstring::npos;
            const bool pathMatch = entry.normalizedPath.find(term) != std::wstring::npos;
            if (!nameMatch && !pathMatch) {
                matches = false;
                break;
            }
            allTermsInName = allTermsInName && nameMatch;
        }
        if (!matches) continue;
        if (allTermsInName) ids->push_back(entry.linkId);
        else secondary.push_back(entry.linkId);
    }
    ids->insert(ids->end(), secondary.begin(), secondary.end());
    return ids;
}

const LinkSearchResult* LinkSearchIndex::FindResult(int linkId) const {
    const auto found = resultIndexById_.find(linkId);
    return found == resultIndexById_.end() ? nullptr : &entries_[found->second].result;
}

std::size_t LinkSearchIndex::size() const noexcept {
    return entries_.size();
}
