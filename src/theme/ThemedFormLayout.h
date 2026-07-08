#pragma once

#include "ThemedUi.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

enum class ThemedRowAlign {
    Left,
    Center,
    Right,
};

enum class ThemedRowDistribution {
    Packed,
    Justify,
};

struct ThemedFormItem {
    int width = 0;
    int height = 0;
    int gapAfter = 0;
};

struct ThemedFormGroup {
    std::vector<ThemedFormItem> items;
    int gapAfter = 0;
};

class ThemedFormLayout {
public:
    explicit ThemedFormLayout(const ThemedUi& ui);

    ThemedFormItem item(int width, int height, int gapAfter = -1) const;
    // 使用旧式固定宽度标签规格；width 小于等于 0 时使用当前布局的默认标签宽度。
    ThemedFormItem label(int width = 0, int gapAfter = -1) const;
    // 使用文本内容自动测量标签宽度；最小宽度统一读取主题 dialog.labelMinWidth。
    ThemedFormItem label(const std::wstring& text, int gapAfter = -1) const;
    // 明确创建固定宽度标签规格；width 小于等于 0 时回退到当前布局的默认标签宽度。
    ThemedFormItem fixedLabel(int width, int gapAfter = -1) const;
    // 计算文本标签所需宽度；非空文本最小宽度统一读取主题 dialog.labelMinWidth，空文本回退到默认标签宽度。
    int labelWidthForText(const std::wstring& text) const;
    // 计算一组文本标签的统一宽度，用于多行 Label + Value 对齐。
    int labelWidthForTexts(std::initializer_list<std::wstring_view> labels) const;
    ThemedFormItem text(int width, int gapAfter = -1) const;
    ThemedFormItem field(int width, int height = 0, int gapAfter = -1) const;
    ThemedFormItem combo(int width, int gapAfter = -1) const;
    ThemedFormItem checkBox(int width, int gapAfter = -1) const;
    ThemedFormItem progress(int width = 0, int gapAfter = -1) const;
    ThemedFormItem button(
        const std::wstring& text,
        ThemedButtonRole role = ThemedButtonRole::Normal,
        ThemedButtonSize size = ThemedButtonSize::Normal,
        ThemedButtonWidthMode widthMode = ThemedButtonWidthMode::Text,
        int fixedWidth = 0,
        int gapAfter = -1) const;
    ThemedFormGroup group(std::initializer_list<ThemedFormItem> items, int gapAfter = -1) const;
    ThemedFormGroup labelField(int labelWidth, int fieldWidth, int gapAfter = -1) const;
    // 创建“自适应标签 + 输入框”分组。
    ThemedFormGroup labelField(const std::wstring& labelText, int fieldWidth, int gapAfter = -1) const;
    ThemedFormGroup labelText(int labelWidth, int textWidth, int gapAfter = -1) const;
    // 创建“自适应标签 + 静态文本”分组。
    ThemedFormGroup labelText(const std::wstring& labelText, int textWidth, int gapAfter = -1) const;
    ThemedFormGroup labelCombo(int labelWidth, int comboWidth, int gapAfter = -1) const;
    // 创建“自适应标签 + 下拉框”分组。
    ThemedFormGroup labelCombo(const std::wstring& labelText, int comboWidth, int gapAfter = -1) const;

    int rowHeight(std::initializer_list<ThemedFormItem> items) const;
    int rowWidth(std::initializer_list<ThemedFormItem> items) const;
    int nextRowY(int y, std::initializer_list<ThemedFormItem> items) const;
    std::vector<RECT> row(int y, ThemedRowAlign align, std::initializer_list<ThemedFormItem> items) const;
    int groupHeight(const ThemedFormGroup& group) const;
    int groupWidth(const ThemedFormGroup& group) const;
    int groupsWidth(std::initializer_list<ThemedFormGroup> groups, ThemedRowDistribution distribution = ThemedRowDistribution::Packed) const;
    int nextRowY(int y, std::initializer_list<ThemedFormGroup> groups) const;
    std::vector<std::vector<RECT>> rowGroups(
        int y,
        ThemedRowAlign align,
        std::initializer_list<ThemedFormGroup> groups) const;
    std::vector<std::vector<RECT>> rowGroups(
        int y,
        ThemedRowAlign align,
        ThemedRowDistribution distribution,
        std::initializer_list<ThemedFormGroup> groups) const;
    std::vector<std::vector<RECT>> justifiedRowGroups(int y, std::initializer_list<ThemedFormGroup> groups) const;

private:
    int defaultGap() const;
    int defaultGroupGap() const;
    int normalizeGap(int gapAfter) const;
    int normalizeGroupGap(int gapAfter) const;
    std::vector<RECT> groupRects(int x, int y, int rowHeight, const ThemedFormGroup& group) const;

    const ThemedUi& ui_;
};
