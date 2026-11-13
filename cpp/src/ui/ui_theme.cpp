#include "ui_theme.hpp"
#include "imgui.h"

namespace ulv {

void UITheme::apply_graphite_iceblue_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Geometric constants adhering to Graphite & Ice Blue Design Spec
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;

    style.WindowPadding     = ImVec2(16.0f, 16.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.ItemSpacing       = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 6.0f);
    style.ScrollbarSize     = 10.0f;
    style.IndentSpacing     = 16.0f;

    // Color Palette: Graphite Black & Ice Blue
    // Base Surface & Windows
    colors[ImGuiCol_WindowBg]              = ThemeColors::PanelBg;      // #151C26
    colors[ImGuiCol_ChildBg]               = ThemeColors::PanelBg;      // #151C26
    colors[ImGuiCol_PopupBg]               = ThemeColors::PanelBg;      // #151C26
    colors[ImGuiCol_Border]                = ThemeColors::Border;       // #2A3747
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Text & Content Hierarchy
    colors[ImGuiCol_Text]                  = ThemeColors::TextPrimary;  // #E8EEF7
    colors[ImGuiCol_TextDisabled]          = ThemeColors::TextMuted;    // #9BAEC3

    // Inputs & Frames (#111923 recessed)
    colors[ImGuiCol_FrameBg]               = ThemeColors::InputBg;      // #111923
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.110f, 0.155f, 0.215f, 1.0f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.145f, 0.198f, 0.270f, 1.0f);

    // Titlebars & Menubars
    colors[ImGuiCol_TitleBg]               = ImVec4(0.055f, 0.075f, 0.105f, 1.0f);
    colors[ImGuiCol_TitleBgActive]         = ThemeColors::PanelBg;
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.050f, 0.070f, 0.095f, 0.85f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.065f, 0.088f, 0.120f, 1.0f);

    // Scrollbars
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.065f, 0.090f, 0.125f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.200f, 0.260f, 0.340f, 0.80f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.260f, 0.340f, 0.440f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.320f, 0.420f, 0.540f, 1.00f);

    // Buttons: Neutral base with Ice Blue accents
    colors[ImGuiCol_Button]                = ThemeColors::ButtonNeutral; // #1C2633
    colors[ImGuiCol_ButtonHovered]         = ThemeColors::ButtonNeutralHover;
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.120f, 0.160f, 0.220f, 1.0f);

    // CheckMarks, Sliders & Highlights (Ice Blue #74B9FF)
    colors[ImGuiCol_CheckMark]             = ThemeColors::AccentIceBlue;
    colors[ImGuiCol_SliderGrab]            = ThemeColors::AccentIceBlue;
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.600f, 0.820f, 1.000f, 1.00f);

    // Navigation Tabs (Segmented dark cards with ice blue indicators)
    colors[ImGuiCol_Tab]                   = ImVec4(0.095f, 0.130f, 0.180f, 0.90f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.130f, 0.180f, 0.245f, 1.00f);
    colors[ImGuiCol_TabSelected]           = ThemeColors::ButtonPrimary; // #1B6CA8
    colors[ImGuiCol_TabSelectedOverline]   = ThemeColors::AccentIceBlue;
    colors[ImGuiCol_TabDimmed]             = ImVec4(0.080f, 0.110f, 0.150f, 0.90f);
    colors[ImGuiCol_TabDimmedSelected]     = ImVec4(0.110f, 0.150f, 0.205f, 1.00f);

    // Headers & Trees
    colors[ImGuiCol_Header]                = ThemeColors::CardBg;
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.145f, 0.198f, 0.270f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ThemeColors::ButtonPrimary;

    // Separators
    colors[ImGuiCol_Separator]             = ThemeColors::Border;
    colors[ImGuiCol_SeparatorHovered]      = ThemeColors::AccentIceBlue;
    colors[ImGuiCol_SeparatorActive]       = ThemeColors::ButtonPrimary;

    // Resize Grasp & Docking
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.200f, 0.260f, 0.340f, 0.40f);
    colors[ImGuiCol_ResizeGripHovered]     = ThemeColors::AccentIceBlue;
    colors[ImGuiCol_ResizeGripActive]      = ThemeColors::ButtonPrimary;
    colors[ImGuiCol_DockingPreview]        = ImVec4(0.106f, 0.424f, 0.659f, 0.40f);
    colors[ImGuiCol_DockingEmptyBg]        = ThemeColors::ViewportBg;
}

} // namespace ulv
