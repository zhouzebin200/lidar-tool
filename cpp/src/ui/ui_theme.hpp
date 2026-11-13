#pragma once
#include <cstdint>
#include "imgui.h"

namespace ulv {

struct ThemeColors {
    // Backgrounds & Surfaces
    static constexpr ImVec4 ViewportBg     = ImVec4(0.047f, 0.067f, 0.094f, 1.0f); // #0C1118
    static constexpr ImVec4 PanelBg        = ImVec4(0.082f, 0.110f, 0.149f, 1.0f); // #151C26
    static constexpr ImVec4 CardBg         = ImVec4(0.110f, 0.149f, 0.200f, 1.0f); // #1C2633
    static constexpr ImVec4 InputBg        = ImVec4(0.067f, 0.098f, 0.137f, 1.0f); // #111923
    static constexpr ImVec4 Border         = ImVec4(0.165f, 0.216f, 0.278f, 1.0f); // #2A3747

    // Typography
    static constexpr ImVec4 TextPrimary    = ImVec4(0.910f, 0.933f, 0.969f, 1.0f); // #E8EEF7
    static constexpr ImVec4 TextMuted      = ImVec4(0.608f, 0.682f, 0.765f, 1.0f); // #9BAEC3
    static constexpr ImVec4 TextDisabled   = ImVec4(0.400f, 0.460f, 0.530f, 1.0f); // #667587

    // Accents & Actions
    static constexpr ImVec4 AccentIceBlue  = ImVec4(0.455f, 0.725f, 1.000f, 1.0f); // #74B9FF
    static constexpr ImVec4 ButtonPrimary  = ImVec4(0.106f, 0.424f, 0.659f, 1.0f); // #1B6CA8
    static constexpr ImVec4 ButtonPrimaryHover = ImVec4(0.180f, 0.525f, 0.871f, 1.0f); // #2E86DE
    static constexpr ImVec4 ButtonNeutral  = ImVec4(0.140f, 0.188f, 0.251f, 1.0f); // Neutral dark
    static constexpr ImVec4 ButtonNeutralHover = ImVec4(0.180f, 0.240f, 0.320f, 1.0f);

    // Status Semantics
    static constexpr ImVec4 SuccessGreen   = ImVec4(0.063f, 0.725f, 0.506f, 1.0f); // #10B981
    static constexpr ImVec4 AmberStatus    = ImVec4(0.961f, 0.620f, 0.043f, 1.0f); // #F59E0B
    static constexpr ImVec4 ErrorRed       = ImVec4(0.937f, 0.267f, 0.267f, 1.0f); // #EF4444
};

class UITheme {
public:
    static void apply_graphite_iceblue_theme();
    static void apply_dark_industrial_theme() {
        apply_graphite_iceblue_theme();
    }
};

} // namespace ulv
