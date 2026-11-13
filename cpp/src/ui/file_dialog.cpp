#include "file_dialog.hpp"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <string>
#include <iostream>

namespace ulv {

std::string show_open_pcap_dialog() {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setCanCreateDirectories:NO];
        [panel setResolvesAliases:YES];
        [panel setTitle:@"导入 PCAP 录包文件"];
        [panel setPrompt:@"导入"];
        [panel setMessage:@"请选择 LiDAR PCAP 数据包文件 (.pcap, .pcapng, .cap)"];

        // Configure file filters (.pcap, .pcapng, .cap)
        if (@available(macOS 11.0, *)) {
            NSMutableArray* allowedTypes = [NSMutableArray array];
            NSArray<NSString*>* extensions = @[@"pcap", @"pcapng", @"cap"];
            for (NSString* ext in extensions) {
                UTType* utType = [UTType typeWithFilenameExtension:ext];
                if (utType) {
                    [allowedTypes addObject:utType];
                }
            }
            [panel setAllowedContentTypes:allowedTypes];
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [panel setAllowedFileTypes:@[@"pcap", @"pcapng", @"cap"]];
#pragma clang diagnostic pop
        }
        [panel setAllowsOtherFileTypes:NO];

        // Bring application to foreground and display panel over GLFW window
        [NSApp activateIgnoringOtherApps:YES];
        [panel makeKeyAndOrderFront:nil];

        // Run modal session (pumps Cocoa event loop, preventing beachball)
        NSModalResponse response = [panel runModal];
        if (response == NSModalResponseOK) {
            NSURL* selectedUrl = [[panel URLs] firstObject];
            if (selectedUrl) {
                NSString* pathStr = [selectedUrl path];
                if (pathStr) {
                    return std::string([pathStr UTF8String]);
                }
            }
        }
        return "";
    }
}

std::string show_open_image_dialog() {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setCanCreateDirectories:NO];
        [panel setResolvesAliases:YES];
        [panel setTitle:@"选择雷达外观检验照片"];
        [panel setPrompt:@"选择"];
        [panel setMessage:@"请选择雷达外观实测照片文件 (.jpg, .jpeg, .png, .webp, .bmp)"];

        if (@available(macOS 11.0, *)) {
            NSMutableArray* allowedTypes = [NSMutableArray array];
            NSArray<NSString*>* extensions = @[@"jpg", @"jpeg", @"png", @"webp", @"bmp"];
            for (NSString* ext in extensions) {
                UTType* utType = [UTType typeWithFilenameExtension:ext];
                if (utType) {
                    [allowedTypes addObject:utType];
                }
            }
            [panel setAllowedContentTypes:allowedTypes];
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [panel setAllowedFileTypes:@[@"jpg", @"jpeg", @"png", @"webp", @"bmp"]];
#pragma clang diagnostic pop
        }
        [panel setAllowsOtherFileTypes:NO];

        [NSApp activateIgnoringOtherApps:YES];
        [panel makeKeyAndOrderFront:nil];

        NSModalResponse response = [panel runModal];
        if (response == NSModalResponseOK) {
            NSURL* selectedUrl = [[panel URLs] firstObject];
            if (selectedUrl) {
                NSString* pathStr = [selectedUrl path];
                if (pathStr) {
                    return std::string([pathStr UTF8String]);
                }
            }
        }
        return "";
    }
}

void open_file_in_system(const std::string& filepath) {
    if (filepath.empty()) return;
    @autoreleasepool {
        NSString* nsStr = [NSString stringWithUTF8String:filepath.c_str()];
        NSURL* url = nil;
        if ([nsStr hasPrefix:@"http://"] || [nsStr hasPrefix:@"https://"] || [nsStr hasPrefix:@"file://"]) {
            url = [NSURL URLWithString:nsStr];
        } else {
            url = [NSURL fileURLWithPath:nsStr];
        }
        if (url) {
            [[NSWorkspace sharedWorkspace] openURL:url];
        }
    }
}

} // namespace ulv

#elif defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <iostream>

namespace ulv {

static std::string wide_to_utf8(const wchar_t* wide) {
    if (!wide || !wide[0]) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), len);
    return out;
}

static std::string open_dialog_w(const wchar_t* filter, const wchar_t* title) {
    wchar_t filename[MAX_PATH * 4] = {0};

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(filename) / sizeof(wchar_t));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        return wide_to_utf8(filename);
    }

    DWORD err = CommDlgExtendedError();
    if (err != 0) {
        std::cerr << "[FileDialog] GetOpenFileNameW failed with CommDlgExtendedError: 0x"
                  << std::hex << err << std::dec << std::endl;
    }
    return "";
}

std::string show_open_pcap_dialog() {
    const wchar_t filter[] = L"PCAP Capture Files (*.pcap;*.pcapng;*.cap)\0*.pcap;*.pcapng;*.cap\0All Files (*.*)\0*.*\0";
    return open_dialog_w(filter, L"导入 PCAP 录包文件");
}

std::string show_open_image_dialog() {
    const wchar_t filter[] = L"Image Files (*.jpg;*.jpeg;*.png;*.webp;*.bmp)\0*.jpg;*.jpeg;*.png;*.webp;*.bmp\0All Files (*.*)\0*.*\0";
    return open_dialog_w(filter, L"选择雷达外观检验照片");
}

void open_file_in_system(const std::string& filepath) {
    if (filepath.empty()) return;
    std::wstring wpath = utf8_to_wide(filepath);
    HINSTANCE res = ShellExecuteW(NULL, L"open", wpath.c_str(), NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)res <= 32) {
        std::cerr << "[FileDialog] ShellExecuteW failed to open: " << filepath
                  << " (code: " << (INT_PTR)res << ")" << std::endl;
    }
}

} // namespace ulv

#else // Linux / POSIX fallback

#include <cstdlib>
#include <cstdio>
#include <string>
#include <iostream>

namespace ulv {

std::string show_open_pcap_dialog() {
    char buffer[1024] = {0};
    FILE* pipe = popen("zenity --file-selection --title=\"导入 PCAP 录包文件\" --file-filter=\"PCAP files (*.pcap, *.pcapng, *.cap) | *.pcap *.pcapng *.cap\" 2>/dev/null", "r");
    if (!pipe) return "";
    std::string result;
    if (fgets(buffer, sizeof(buffer), pipe)) {
        result = buffer;
    }
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

std::string show_open_image_dialog() {
    char buffer[1024] = {0};
    FILE* pipe = popen("zenity --file-selection --title=\"选择雷达外观检验照片\" --file-filter=\"Images (*.jpg, *.jpeg, *.png, *.webp, *.bmp) | *.jpg *.jpeg *.png *.webp *.bmp\" 2>/dev/null", "r");
    if (!pipe) return "";
    std::string result;
    if (fgets(buffer, sizeof(buffer), pipe)) {
        result = buffer;
    }
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

void open_file_in_system(const std::string& filepath) {
    if (filepath.empty()) return;
    std::string cmd = "xdg-open \"" + filepath + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

} // namespace ulv

#endif
