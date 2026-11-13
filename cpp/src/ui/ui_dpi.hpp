#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <cfloat>

namespace ulv {

// Scales ImGui display metrics and queued pointer coordinates to logical units
// on platforms where GLFW reports window/mouse coordinates in physical pixels.
class UIDpiAdapter {
public:
    void before_frame(float coordinate_scale) const {
        if (coordinate_scale <= 1.0f) return;

        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx) return;

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize.x /= coordinate_scale;
        io.DisplaySize.y /= coordinate_scale;
        io.DisplayFramebufferScale.x *= coordinate_scale;
        io.DisplayFramebufferScale.y *= coordinate_scale;

        if (io.MousePos.x != -FLT_MAX && io.MousePos.y != -FLT_MAX) {
            io.MousePos.x /= coordinate_scale;
            io.MousePos.y /= coordinate_scale;
        }

        for (ImGuiInputEvent& ev : ctx->InputEventsQueue) {
            if (ev.Type == ImGuiInputEventType_MousePos &&
                ev.MousePos.PosX != -FLT_MAX && ev.MousePos.PosY != -FLT_MAX) {
                ev.MousePos.PosX /= coordinate_scale;
                ev.MousePos.PosY /= coordinate_scale;
            }
        }
    }
};

} // namespace ulv
