# UI Review Fixes Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement task by task.

**Goal:** Resolve the eight approved review findings while preserving the graphite/ice-blue reference design.
**Architecture:** Keep the existing ImGui workspace; bind presentation to actual telemetry, use renderer-matched legend sampling, and route playback/camera actions through the application state. Add a headless ImGui regression target.
**Tech Stack:** C++17, Dear ImGui, GLFW, OpenGL, CMake.
**Spec:** `docs/superpowers/specs/2026-11-02-graphite-ui-design.md` and the eight findings approved in chat.

## Global Constraints

- Preserve unrelated uncommitted work; implement in the current workspace because cpp/ itself is untracked. No bulk git add or commits.
- Maintain macOS and Windows application-content parity; native dialogs remain native.
- Missing telemetry must say unknown; never substitute mockup values.
- Use packet progress until genuine PCAP time indexing exists.

## Review Focus

- No device, changed device/IP, or failed diagnostics must never expose default report identity as telemetry.
- All five color modes must agree with renderer normalization, gamma and palette.
- Replay controls must not rotate the camera or change a live stream implicitly.
- 1100×720 and normal windows must keep controls contained; reports must occlude overlays.
- Packaged font and geometric icons must not depend on OS glyph fallback.

### Task 1: Behavioral regression checks
- [x] Add `cpp/tests/test_ui_workspace.cpp` and a CMake headless ImGui test target. Log real UI text and inspect ImGui window/draw state to cover unknown device, actual throughput, overlay layering, narrow timeline containment and mouse capture.
- [x] Run the target and observe failures before edits.

### Task 2: Data and legends
- [x] Update `cpp/src/ui/ui_workspace.*`: use diagnostic identity only after successful query for the same target; show actual network metrics; neutral unknowns; configured port badges. Fix PTP's hardcoded status encountered in the same UI.
- [x] Add `cpp/src/renderer/color_mapping.hpp`, shared renderer palette constants and CPU sampling/legend range helpers, with numerical tests for all modes.
- [x] Use packet index progress; remove fabricated time labels.

### Task 3: Interaction and geometry
- [x] Share layout constants; draw clipped overlays in their own noninteractive window, hide them behind report, and avoid viewport foreground drawing.
- [x] Restrict timeline to replay. Pass mode by reference. Emit seek requests and reset decoder/frame queue when applying them; decode one paused preview frame.
- [x] Apply camera input after UI construction and honor current UI capture. Use measured timeline controls and two-line port tiles. Add nav underline, camera preset selection and status strip.

### Task 4: Assets and verification
- [x] Bundle Noto Sans SC plus OFL; copy resources beside both executables and into macOS bundle; remove emoji from workspace controls and use geometric drawn icons.
- [x] Build macOS, Windows and headless tests; run existing suite plus UI regressions. Launch macOS and inspect real screenshots and controls if desktop access is available.
- [x] Request one independent final review of changed files, fix material issues, and record verification/limitations.

## Decisions

- Execute directly as authorized by the user's request to implement the reviewed changes; no additional design approval is needed.
- Keep PCAP progress as packets instead of adding a new timestamp index subsystem.

## Execution outcome

Completed implementation, one independent review and material EOF-preview fix. macOS UI 14/14 and existing tests 74/74; Windows PD UI 14/14 and 30-frame PCAP verification passed. Windows DPI input gained two additional checks. Actual GUI rendering was observed; PD console mouse automation was not treated as a full end-to-end acceptance signal. Hardware UDP/PTP remains unverified. See `docs/verification-2026-11-03/README.md`.
