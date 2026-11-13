#pragma once

#include <string>

namespace ulv {

/**
 * @brief Displays the operating system's native file picker dialog to select LiDAR PCAP files.
 *        - macOS: Cocoa NSOpenPanel with UTType / allowedContentTypes filter (.pcap, .pcapng, .cap).
 *        - Windows: Win32 GetOpenFileNameA with OFN filter.
 *        - Linux: Zenity fallback.
 * @return Absolute filesystem path of the selected file, or an empty string if cancelled.
 */
std::string show_open_pcap_dialog();

/**
 * @brief Displays the operating system's native file picker dialog to select image files (JPG, PNG, WebP, BMP).
 * @return Absolute filesystem path of the selected file, or an empty string if cancelled.
 */
std::string show_open_image_dialog();

/**
 * @brief Opens a file in the operating system's default viewer/application.
 *        - macOS: [[NSWorkspace sharedWorkspace] openURL:url]
 *        - Windows: ShellExecuteA(..., "open", ...)
 *        - Linux: xdg-open
 * @param filepath Path to the file or URL.
 */
void open_file_in_system(const std::string& filepath);

} // namespace ulv
