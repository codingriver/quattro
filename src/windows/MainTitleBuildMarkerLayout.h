#pragma once

#include <algorithm>

struct MainTitleBuildMarkerLayout {
    float nameEnd = 0.0f;
    float markerLeft = 0.0f;
};

inline MainTitleBuildMarkerLayout CalculateMainTitleBuildMarkerLayout(
    float textLeft,
    float textEnd,
    float markerWidth,
    float gap) {
    const float markerLeft = std::max(textLeft, textEnd - std::max(0.0f, markerWidth));
    return MainTitleBuildMarkerLayout{
        std::max(textLeft, markerLeft - std::max(0.0f, gap)),
        markerLeft,
    };
}
