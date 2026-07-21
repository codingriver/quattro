#pragma once

#include "Theme.h"

#include <algorithm>
#include <windows.h>

enum class DialogLayoutKind {
    Standard,
    Compact,
    Mini,
    Overlay,
};

struct DialogLayoutMetrics {
    int contentInsetX = 24;
    int contentInsetY = 24;
    int labelMinWidth = 20;
    int labelWidth = 74;
    int labelGap = 6;
    int rowGap = 8;
    int sectionGap = 16;
    int footerGap = 16;
    int footerInsetY = 24;
    int controlGapX = 12;
    int footerButtonWidth = 76;
    int footerButtonGap = 16;
    int footerButtonHeight = 32;
    int fieldX = 104;

    int RowStep(int controlHeight) const {
        return controlHeight + rowGap;
    }

    int FooterY(int contentBottom) const {
        return contentBottom + footerGap;
    }

    int FooterButtonY(int clientHeight, int buttonHeight) const {
        return clientHeight - footerInsetY - buttonHeight;
    }

    int FooterButtonX(int dialogWidth, int buttonIndex, int buttonCount) const {
        const int totalWidth = buttonCount * footerButtonWidth + std::max(0, buttonCount - 1) * footerButtonGap;
        return dialogWidth - contentInsetX - totalWidth + buttonIndex * (footerButtonWidth + footerButtonGap);
    }

    int CenteredGroupX(int dialogWidth, int groupWidth) const {
        const int minX = contentInsetX;
        const int maxX = std::max(minX, dialogWidth - contentInsetX - groupWidth);
        return std::max(minX, std::min((dialogWidth - groupWidth) / 2, maxX));
    }

    RECT ContentRect(int width, int height) const {
        return RECT{contentInsetX, contentInsetY, width - contentInsetX, height - contentInsetY};
    }
};

inline int ScaleDialogMetric(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

inline DialogLayoutMetrics ScaleDialogLayoutMetrics(DialogLayoutMetrics metrics, UINT dpi) {
    metrics.contentInsetX = ScaleDialogMetric(metrics.contentInsetX, dpi);
    metrics.contentInsetY = ScaleDialogMetric(metrics.contentInsetY, dpi);
    metrics.labelMinWidth = ScaleDialogMetric(metrics.labelMinWidth, dpi);
    metrics.labelWidth = ScaleDialogMetric(metrics.labelWidth, dpi);
    metrics.labelGap = ScaleDialogMetric(metrics.labelGap, dpi);
    metrics.rowGap = ScaleDialogMetric(metrics.rowGap, dpi);
    metrics.sectionGap = ScaleDialogMetric(metrics.sectionGap, dpi);
    metrics.footerGap = ScaleDialogMetric(metrics.footerGap, dpi);
    metrics.footerInsetY = ScaleDialogMetric(metrics.footerInsetY, dpi);
    metrics.controlGapX = ScaleDialogMetric(metrics.controlGapX, dpi);
    metrics.footerButtonWidth = ScaleDialogMetric(metrics.footerButtonWidth, dpi);
    metrics.footerButtonGap = ScaleDialogMetric(metrics.footerButtonGap, dpi);
    metrics.footerButtonHeight = ScaleDialogMetric(metrics.footerButtonHeight, dpi);
    metrics.fieldX = metrics.contentInsetX + metrics.labelWidth + metrics.labelGap;
    return metrics;
}

inline int DialogLayoutMetric(const Theme& theme, const wchar_t* name, float fallback) {
    return static_cast<int>(theme.metric(L"dialog", name, fallback));
}

inline DialogLayoutMetrics GetDialogLayoutMetrics(const Theme& theme, DialogLayoutKind kind) {
    DialogLayoutMetrics metrics{};
    switch (kind) {
    case DialogLayoutKind::Compact:
        metrics.contentInsetX = DialogLayoutMetric(theme, L"compactContentInsetX", 12.0f);
        metrics.contentInsetY = DialogLayoutMetric(theme, L"compactContentInsetY", 10.0f);
        metrics.labelWidth = DialogLayoutMetric(theme, L"compactLabelWidth", 84.0f);
        metrics.labelGap = DialogLayoutMetric(theme, L"compactLabelGap", 10.0f);
        metrics.rowGap = DialogLayoutMetric(theme, L"compactRowGap", 6.0f);
        metrics.sectionGap = DialogLayoutMetric(theme, L"compactSectionGap", 12.0f);
        metrics.footerGap = DialogLayoutMetric(theme, L"compactFooterGap", 12.0f);
        metrics.footerInsetY = DialogLayoutMetric(theme, L"compactFooterInsetY", 16.0f);
        metrics.controlGapX = DialogLayoutMetric(theme, L"compactControlGapX", 10.0f);
        metrics.footerButtonWidth = DialogLayoutMetric(theme, L"compactFooterButtonWidth", 86.0f);
        metrics.footerButtonGap = DialogLayoutMetric(theme, L"compactFooterButtonGap", 10.0f);
        break;
    case DialogLayoutKind::Mini:
        metrics.contentInsetX = DialogLayoutMetric(theme, L"miniContentInsetX", 24.0f);
        metrics.contentInsetY = DialogLayoutMetric(theme, L"miniContentInsetY", 18.0f);
        metrics.labelWidth = DialogLayoutMetric(theme, L"miniLabelWidth", 0.0f);
        metrics.labelGap = DialogLayoutMetric(theme, L"miniLabelGap", 0.0f);
        metrics.rowGap = DialogLayoutMetric(theme, L"miniRowGap", 8.0f);
        metrics.sectionGap = DialogLayoutMetric(theme, L"miniSectionGap", 12.0f);
        metrics.footerGap = DialogLayoutMetric(theme, L"miniFooterGap", 12.0f);
        metrics.footerInsetY = DialogLayoutMetric(theme, L"miniFooterInsetY", 16.0f);
        metrics.controlGapX = DialogLayoutMetric(theme, L"miniControlGapX", 12.0f);
        metrics.footerButtonWidth = DialogLayoutMetric(theme, L"miniFooterButtonWidth", 72.0f);
        metrics.footerButtonGap = DialogLayoutMetric(theme, L"miniFooterButtonGap", 16.0f);
        break;
    case DialogLayoutKind::Overlay:
        metrics.contentInsetX = DialogLayoutMetric(theme, L"overlayContentInsetX", 24.0f);
        metrics.contentInsetY = DialogLayoutMetric(theme, L"overlayContentInsetY", 16.0f);
        metrics.labelWidth = DialogLayoutMetric(theme, L"overlayLabelWidth", 0.0f);
        metrics.labelGap = DialogLayoutMetric(theme, L"overlayLabelGap", 0.0f);
        metrics.rowGap = DialogLayoutMetric(theme, L"overlayRowGap", 8.0f);
        metrics.sectionGap = DialogLayoutMetric(theme, L"overlaySectionGap", 12.0f);
        metrics.footerGap = DialogLayoutMetric(theme, L"overlayFooterGap", 8.0f);
        metrics.footerInsetY = DialogLayoutMetric(theme, L"overlayFooterInsetY", 16.0f);
        metrics.controlGapX = DialogLayoutMetric(theme, L"overlayControlGapX", 10.0f);
        metrics.footerButtonWidth = DialogLayoutMetric(theme, L"overlayFooterButtonWidth", 76.0f);
        metrics.footerButtonGap = DialogLayoutMetric(theme, L"overlayFooterButtonGap", 12.0f);
        break;
    case DialogLayoutKind::Standard:
    default:
        metrics.contentInsetX = DialogLayoutMetric(theme, L"contentInsetX", 28.0f);
        metrics.contentInsetY = DialogLayoutMetric(theme, L"contentInsetY", 24.0f);
        metrics.labelWidth = DialogLayoutMetric(theme, L"labelWidth", 74.0f);
        metrics.labelGap = DialogLayoutMetric(theme, L"labelGap", 6.0f);
        metrics.rowGap = DialogLayoutMetric(theme, L"rowGap", 8.0f);
        metrics.sectionGap = DialogLayoutMetric(theme, L"sectionGap", 16.0f);
        metrics.footerGap = DialogLayoutMetric(theme, L"footerGap", 16.0f);
        metrics.footerInsetY = DialogLayoutMetric(theme, L"footerInsetY", 24.0f);
        metrics.controlGapX = DialogLayoutMetric(theme, L"controlGapX", 12.0f);
        metrics.footerButtonWidth = DialogLayoutMetric(theme, L"footerButtonWidth", 76.0f);
        metrics.footerButtonGap = DialogLayoutMetric(theme, L"footerButtonGap", 16.0f);
        break;
    }
    metrics.labelMinWidth = DialogLayoutMetric(theme, L"labelMinWidth", 20.0f);
    metrics.footerButtonHeight = static_cast<int>(theme.metric(L"button", L"largeHeight", 32.0f));
    metrics.fieldX = metrics.contentInsetX + metrics.labelWidth + metrics.labelGap;
    return metrics;
}
