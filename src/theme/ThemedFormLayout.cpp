#include "ThemedFormLayout.h"

#include "ThemedControls.h"

#include <algorithm>

ThemedFormLayout::ThemedFormLayout(const ThemedUi& ui)
    : ui_(ui) {}

ThemedFormItem ThemedFormLayout::item(int width, int height, int gapAfter) const {
    return ThemedFormItem{std::max(0, width), std::max(0, height), normalizeGap(gapAfter)};
}

ThemedFormItem ThemedFormLayout::label(int width, int gapAfter) const {
    return fixedLabel(width > 0 ? width : labelWidthForText(L""), gapAfter);
}

ThemedFormItem ThemedFormLayout::label(const std::wstring& text, int gapAfter) const {
    return fixedLabel(labelWidthForText(text), gapAfter);
}

ThemedFormItem ThemedFormLayout::fixedLabel(int width, int gapAfter) const {
    const int labelWidth = width > 0 ? width : labelWidthForText(L"");
    return item(labelWidth, ui_.labelHeight(), gapAfter < 0 ? ui_.layout().labelGap : gapAfter);
}

int ThemedFormLayout::labelWidthForText(const std::wstring& text) const {
    const int defaultMin = std::max(0, ui_.layout().labelMinWidth);
    const int measured = text.empty() ? 0 : ui_.textWidth(text);
    const int fallback = ui_.layout().labelWidth > 0 ? ui_.layout().labelWidth : ui_.labelHeight() * 2;
    if (text.empty()) {
        return std::max(defaultMin, fallback);
    }
    return std::max(measured, defaultMin);
}

int ThemedFormLayout::labelWidthForTexts(std::initializer_list<std::wstring_view> labels) const {
    int width = 0;
    for (std::wstring_view label : labels) {
        width = std::max(width, labelWidthForText(std::wstring(label)));
    }
    return width;
}

ThemedFormItem ThemedFormLayout::text(int width, int gapAfter) const {
    return item(width, ui_.labelHeight(), gapAfter);
}

ThemedFormItem ThemedFormLayout::field(int width, int height, int gapAfter) const {
    const int fieldHeight = height > 0 ? height : ThemedControls::FieldFrameHeight(ui_.theme());
    return item(width, fieldHeight, gapAfter);
}

ThemedFormItem ThemedFormLayout::combo(int width, int gapAfter) const {
    return item(width, ThemedControls::EditFrameHeight(ui_.theme()), gapAfter);
}

ThemedFormItem ThemedFormLayout::checkBox(int width, int gapAfter) const {
    return item(width, ThemedControls::CheckBoxHeight(ui_.theme()), gapAfter);
}

ThemedFormItem ThemedFormLayout::progress(int width, int gapAfter) const {
    const int progressWidth = width > 0 ? width : ui_.contentWidth();
    return item(progressWidth, ui_.progressBarHeight(), gapAfter);
}

ThemedFormItem ThemedFormLayout::button(
    const std::wstring& text,
    ThemedButtonRole role,
    ThemedButtonSize size,
    ThemedButtonWidthMode widthMode,
    int fixedWidth,
    int gapAfter) const {
    return item(
        ui_.buttonWidth(text, role, size, widthMode, fixedWidth),
        ui_.buttonHeight(role, size),
        gapAfter);
}

ThemedFormGroup ThemedFormLayout::group(std::initializer_list<ThemedFormItem> items, int gapAfter) const {
    return ThemedFormGroup{std::vector<ThemedFormItem>(items.begin(), items.end()), normalizeGroupGap(gapAfter)};
}

ThemedFormGroup ThemedFormLayout::labelField(int labelWidth, int fieldWidth, int gapAfter) const {
    return group({fixedLabel(labelWidth), field(fieldWidth)}, gapAfter);
}

ThemedFormGroup ThemedFormLayout::labelField(const std::wstring& labelText, int fieldWidth, int gapAfter) const {
    return group({label(labelText), field(fieldWidth)}, gapAfter);
}

ThemedFormGroup ThemedFormLayout::labelText(int labelWidth, int textWidth, int gapAfter) const {
    return group({fixedLabel(labelWidth), text(textWidth)}, gapAfter);
}

ThemedFormGroup ThemedFormLayout::labelText(const std::wstring& labelText, int textWidth, int gapAfter) const {
    return group({label(labelText), text(textWidth)}, gapAfter);
}

ThemedFormGroup ThemedFormLayout::labelCombo(int labelWidth, int comboWidth, int gapAfter) const {
    return group({fixedLabel(labelWidth), combo(comboWidth)}, gapAfter);
}

ThemedFormGroup ThemedFormLayout::labelCombo(const std::wstring& labelText, int comboWidth, int gapAfter) const {
    return group({label(labelText), combo(comboWidth)}, gapAfter);
}

int ThemedFormLayout::rowHeight(std::initializer_list<ThemedFormItem> items) const {
    int height = 0;
    for (const auto& item : items) {
        height = std::max(height, item.height);
    }
    return height;
}

int ThemedFormLayout::rowWidth(std::initializer_list<ThemedFormItem> items) const {
    int width = 0;
    int index = 0;
    const int count = static_cast<int>(items.size());
    for (const auto& item : items) {
        width += item.width;
        if (index + 1 < count) {
            width += item.gapAfter;
        }
        ++index;
    }
    return width;
}

int ThemedFormLayout::nextRowY(int y, std::initializer_list<ThemedFormItem> items) const {
    return ui_.nextRowY(y, rowHeight(items));
}

std::vector<RECT> ThemedFormLayout::row(int y, ThemedRowAlign align, std::initializer_list<ThemedFormItem> items) const {
    std::vector<ThemedFormItem> copied(items.begin(), items.end());
    std::vector<RECT> rects;
    rects.reserve(copied.size());

    int totalWidth = 0;
    for (std::size_t i = 0; i < copied.size(); ++i) {
        totalWidth += copied[i].width;
        if (i + 1 < copied.size()) {
            totalWidth += copied[i].gapAfter;
        }
    }

    int x = ui_.contentLeft();
    if (align == ThemedRowAlign::Center) {
        x = ui_.centeredGroupX(totalWidth);
    } else if (align == ThemedRowAlign::Right) {
        x = std::max(ui_.contentLeft(), ui_.contentLeft() + ui_.contentWidth() - totalWidth);
    }

    const int height = rowHeight(items);
    for (std::size_t i = 0; i < copied.size(); ++i) {
        const int top = y + (height - copied[i].height) / 2;
        rects.push_back(RECT{x, top, x + copied[i].width, top + copied[i].height});
        x += copied[i].width;
        if (i + 1 < copied.size()) {
            x += copied[i].gapAfter;
        }
    }
    return rects;
}

int ThemedFormLayout::groupHeight(const ThemedFormGroup& group) const {
    int height = 0;
    for (const auto& item : group.items) {
        height = std::max(height, item.height);
    }
    return height;
}

int ThemedFormLayout::groupWidth(const ThemedFormGroup& group) const {
    int width = 0;
    for (std::size_t i = 0; i < group.items.size(); ++i) {
        width += group.items[i].width;
        if (i + 1 < group.items.size()) {
            width += group.items[i].gapAfter;
        }
    }
    return width;
}

int ThemedFormLayout::groupsWidth(std::initializer_list<ThemedFormGroup> groups, ThemedRowDistribution distribution) const {
    int width = 0;
    int index = 0;
    const int count = static_cast<int>(groups.size());
    for (const auto& group : groups) {
        width += groupWidth(group);
        if (index + 1 < count) {
            width += distribution == ThemedRowDistribution::Justify ? defaultGroupGap() : group.gapAfter;
        }
        ++index;
    }
    return width;
}

int ThemedFormLayout::nextRowY(int y, std::initializer_list<ThemedFormGroup> groups) const {
    int height = 0;
    for (const auto& group : groups) {
        height = std::max(height, groupHeight(group));
    }
    return ui_.nextRowY(y, height);
}

std::vector<std::vector<RECT>> ThemedFormLayout::rowGroups(
    int y,
    ThemedRowAlign align,
    std::initializer_list<ThemedFormGroup> groups) const {
    return rowGroups(y, align, ThemedRowDistribution::Packed, groups);
}

std::vector<std::vector<RECT>> ThemedFormLayout::rowGroups(
    int y,
    ThemedRowAlign align,
    ThemedRowDistribution distribution,
    std::initializer_list<ThemedFormGroup> groups) const {
    std::vector<ThemedFormGroup> copied(groups.begin(), groups.end());
    std::vector<std::vector<RECT>> rects;
    rects.reserve(copied.size());
    if (copied.empty()) {
        return rects;
    }

    int fixedWidth = 0;
    int height = 0;
    for (const auto& group : copied) {
        fixedWidth += groupWidth(group);
        height = std::max(height, groupHeight(group));
    }

    int groupGap = defaultGroupGap();
    if (distribution == ThemedRowDistribution::Justify && copied.size() > 1) {
        const int available = ui_.contentWidth();
        if (fixedWidth < available) {
            groupGap = std::max(defaultGroupGap(), (available - fixedWidth) / static_cast<int>(copied.size() - 1));
        }
    }

    int totalWidth = fixedWidth;
    for (std::size_t i = 0; i + 1 < copied.size(); ++i) {
        totalWidth += distribution == ThemedRowDistribution::Justify ? groupGap : copied[i].gapAfter;
    }

    int x = ui_.contentLeft();
    if (distribution != ThemedRowDistribution::Justify) {
        if (align == ThemedRowAlign::Center) {
            x = ui_.centeredGroupX(totalWidth);
        } else if (align == ThemedRowAlign::Right) {
            x = std::max(ui_.contentLeft(), ui_.contentLeft() + ui_.contentWidth() - totalWidth);
        }
    }

    for (std::size_t i = 0; i < copied.size(); ++i) {
        rects.push_back(groupRects(x, y, height, copied[i]));
        x += groupWidth(copied[i]);
        if (i + 1 < copied.size()) {
            x += distribution == ThemedRowDistribution::Justify ? groupGap : copied[i].gapAfter;
        }
    }
    return rects;
}

std::vector<std::vector<RECT>> ThemedFormLayout::justifiedRowGroups(int y, std::initializer_list<ThemedFormGroup> groups) const {
    return rowGroups(y, ThemedRowAlign::Left, ThemedRowDistribution::Justify, groups);
}

int ThemedFormLayout::defaultGap() const {
    return ui_.layout().controlGapX;
}

int ThemedFormLayout::defaultGroupGap() const {
    return ui_.layout().controlGapX * 2;
}

int ThemedFormLayout::normalizeGap(int gapAfter) const {
    return gapAfter < 0 ? defaultGap() : gapAfter;
}

int ThemedFormLayout::normalizeGroupGap(int gapAfter) const {
    return gapAfter < 0 ? defaultGroupGap() : gapAfter;
}

std::vector<RECT> ThemedFormLayout::groupRects(int x, int y, int rowHeight, const ThemedFormGroup& group) const {
    std::vector<RECT> rects;
    rects.reserve(group.items.size());
    for (std::size_t i = 0; i < group.items.size(); ++i) {
        const auto& item = group.items[i];
        const int top = y + (rowHeight - item.height) / 2;
        rects.push_back(RECT{x, top, x + item.width, top + item.height});
        x += item.width;
        if (i + 1 < group.items.size()) {
            x += item.gapAfter;
        }
    }
    return rects;
}
