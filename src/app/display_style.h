// 3D view display styles (spec/app-ux.md §3.4а, S2-VIEW-001): what the renderer draws and how in each style.
#pragma once

#include <optional>
#include <string_view>

namespace construe::app {

enum class DisplayStyle { ShadedEdges, Shaded, HiddenRemoved, HiddenDashed, Wireframe };

inline constexpr DisplayStyle ALL_DISPLAY_STYLES[] = {DisplayStyle::ShadedEdges, DisplayStyle::Shaded, DisplayStyle::HiddenRemoved,
                                                      DisplayStyle::HiddenDashed, DisplayStyle::Wireframe};

// Style layout — a row of the §3.4а table.
struct StyleLayout {
    bool drawFaces = true;       // faces are drawn (in wireframe — only hover and selection highlight)
    bool shadedFaces = true;     // shading and part colors; otherwise COLOR_HLR_FACE fill without lighting
    bool visibleEdges = true;    // visible edges
    bool hiddenEdges = false;    // hidden edges are drawn
    bool hiddenDashed = false;   // hidden edges — dashed COLOR_EDGE_HIDDEN (otherwise like visible ones)
    bool edgesOverFacesInId = false;  // edges in the ID buffer without depth test, on top of faces
};

inline StyleLayout styleLayout(DisplayStyle s) {
    StyleLayout l;
    switch (s) {
    case DisplayStyle::ShadedEdges: break;
    case DisplayStyle::Shaded: l.visibleEdges = false; break;
    case DisplayStyle::HiddenRemoved: l.shadedFaces = false; break;
    case DisplayStyle::HiddenDashed: l.shadedFaces = false, l.hiddenEdges = true, l.hiddenDashed = true; break;
    case DisplayStyle::Wireframe: l.drawFaces = false, l.shadedFaces = false, l.hiddenEdges = true, l.edgesOverFacesInId = true; break;
    }
    return l;
}

// Style name (§3.4а): settings key and suffix of actions/icons.
inline const char* styleName(DisplayStyle s) {
    switch (s) {
    case DisplayStyle::ShadedEdges: return "shadedEdges";
    case DisplayStyle::Shaded: return "shaded";
    case DisplayStyle::HiddenRemoved: return "hiddenRemoved";
    case DisplayStyle::HiddenDashed: return "hiddenDashed";
    case DisplayStyle::Wireframe: return "wireframe";
    }
    return "shadedEdges";
}

inline std::optional<DisplayStyle> styleFromName(std::string_view n) {
    for (DisplayStyle s : ALL_DISPLAY_STYLES)
        if (n == styleName(s)) return s;
    return std::nullopt;
}

}  // namespace construe::app
