#pragma once

#include <d2d1.h>
#include <windows.h>

namespace quattro::windows {

inline POINT RectCenterPoint(const D2D1_RECT_F& rect) {
    return POINT{
        static_cast<LONG>((rect.left + rect.right) * 0.5f),
        static_cast<LONG>((rect.top + rect.bottom) * 0.5f),
    };
}

template <typename AreaRange, typename MatchesArea>
bool TryMatchedAreaCenterPoint(const AreaRange& areas, MatchesArea matchesArea, POINT& point) {
    for (const auto& area : areas) {
        if (!matchesArea(area)) {
            continue;
        }
        point = RectCenterPoint(area.rect);
        return true;
    }
    return false;
}

}
