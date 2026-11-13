#include "ptc_client.hpp"

#include <iostream>
#include <sstream>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
using sock_ssize_t = int;
#define CLOSE_SOCKET(s) ::closesocket(s)
#define POLL_FN WSAPoll
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
using socket_t = int;
using sock_ssize_t = ssize_t;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif
#define CLOSE_SOCKET(s) ::close(s)
#define POLL_FN poll
#endif

namespace ulv {

namespace {

#if defined(_WIN32)
struct WinsockScope {
    WinsockScope() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockScope() {
        WSACleanup();
    }
};
static void ensure_winsock() {
    static WinsockScope s_wsa;
}
#else
static void ensure_winsock() {}
#endif

inline int get_last_error() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

inline bool is_in_progress(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return err == EINPROGRESS || err == EWOULDBLOCK || err == EAGAIN;
#endif
}

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

inline int32_t read_be32_signed(const uint8_t* p) {
    return static_cast<int32_t>(read_be32(p));
}

inline uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

inline void write_be32(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(val & 0xFF);
}

inline std::string extract_str(const uint8_t* data, size_t max_len) {
    size_t len = 0;
    while (len < max_len && data[len] != '\0') {
        len++;
    }
    while (len > 0 && (data[len - 1] == ' ' || data[len - 1] == '\r' || data[len - 1] == '\n' || data[len - 1] == '\t')) {
        len--;
    }
    return std::string(reinterpret_cast<const char*>(data), len);
}

inline std::string format_mac(const uint8_t* p) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  p[0], p[1], p[2], p[3], p[4], p[5]);
    return std::string(buf);
}

inline std::string format_ip(const uint8_t* p) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return std::string(buf);
}

// RAII cross-platform non-blocking TCP socket wrapper
class TcpSocket {
public:
    socket_t fd_ = INVALID_SOCKET;

    TcpSocket() = default;
    ~TcpSocket() { close(); }

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    void close() {
        if (fd_ != INVALID_SOCKET) {
            CLOSE_SOCKET(fd_);
            fd_ = INVALID_SOCKET;
        }
    }

    bool connect(const std::string& ip, uint16_t port, int timeout_ms) {
        ensure_winsock();
        close();

        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == INVALID_SOCKET) return false;

        // Set non-blocking mode
#if defined(_WIN32)
        u_long mode = 1;
        if (ioctlsocket(fd_, FIONBIO, &mode) != 0) {
            close();
            return false;
        }
#else
        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags == -1 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
            close();
            return false;
        }
#endif

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            close();
            return false;
        }

        int res = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        if (res == 0) {
            return true; // Immediate connection (loopback)
        }

        int err = get_last_error();
        if (!is_in_progress(err)) {
            close();
            return false;
        }

        // Wait for connection completion via poll on POLLOUT
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;

        int poll_ret = POLL_FN(&pfd, 1, timeout_ms);
        if (poll_ret <= 0) {
            close();
            return false; // Timeout (0) or error (<0)
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close();
            return false;
        }

        if (!(pfd.revents & POLLOUT)) {
            close();
            return false;
        }

        // Check socket error status
        int so_error = 0;
        socklen_t optlen = sizeof(so_error);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &optlen) < 0 || so_error != 0) {
            close();
            return false;
        }

        return true;
    }

    bool send_exact(const void* data, size_t len, int timeout_ms) {
        if (fd_ == INVALID_SOCKET) return false;
        const char* ptr = reinterpret_cast<const char*>(data);
        size_t total_sent = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (total_sent < len) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            int ret = POLL_FN(&pfd, 1, remaining);
            if (ret <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;

            sock_ssize_t n = ::send(fd_, ptr + total_sent, static_cast<int>(len - total_sent), 0);
            if (n < 0) {
                int err = get_last_error();
                if (is_in_progress(err)) continue;
                return false;
            }
            if (n == 0) return false;
            total_sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool recv_exact(void* data, size_t len, int timeout_ms) {
        if (fd_ == INVALID_SOCKET) return false;
        char* ptr = reinterpret_cast<char*>(data);
        size_t total_recv = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (total_recv < len) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLIN;
            int ret = POLL_FN(&pfd, 1, remaining);
            if (ret <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;

            sock_ssize_t n = ::recv(fd_, ptr + total_recv, static_cast<int>(len - total_recv), 0);
            if (n < 0) {
                int err = get_last_error();
                if (is_in_progress(err)) continue;
                return false;
            }
            if (n == 0) return false; // Peer closed connection
            total_recv += static_cast<size_t>(n);
        }
        return true;
    }

    bool recv_all_until_close(std::string& out, int timeout_ms, size_t max_bytes = 256 * 1024) {
        if (fd_ == INVALID_SOCKET) return false;
        out.clear();
        char buf[4096];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (out.size() < max_bytes) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLIN;
            int ret = POLL_FN(&pfd, 1, remaining);
            if (ret <= 0 || (pfd.revents & (POLLERR | POLLNVAL))) break;

            sock_ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n < 0) {
                int err = get_last_error();
                if (is_in_progress(err)) continue;
                break;
            }
            if (n == 0) {
                return true; // EOF, normal close
            }
            out.append(buf, static_cast<size_t>(n));
        }
        return !out.empty();
    }
};

} // namespace

std::string HesaiPtcClient::synthesize_model_name(const std::string& sn, const std::string& pn) {
    std::string sn_up = sn;
    std::string pn_up = pn;
    for (auto& c : sn_up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (auto& c : pn_up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (sn_up.find("AT128P") != std::string::npos || pn_up.find("AT128P") != std::string::npos || pn_up.find("E3X") != std::string::npos) {
        return "Hesai AT128P";
    }
    if (sn_up.find("AT360") != std::string::npos || pn_up.find("AT360") != std::string::npos) {
        return "Hesai AT360";
    }
    if (sn_up.find("ATXP") != std::string::npos || pn_up.find("ATXP") != std::string::npos) {
        return "Hesai ATXP";
    }
    if (sn_up.find("ATX") != std::string::npos || pn_up.find("ATX") != std::string::npos) {
        return "Hesai ATX";
    }
    if (sn_up.find("ETXP") != std::string::npos || pn_up.find("ETXP") != std::string::npos ||
        sn_up.find("ETX") != std::string::npos || pn_up.find("ETX") != std::string::npos) {
        return "Hesai ETX";
    }
    if (sn_up.find("FT120") != std::string::npos || pn_up.find("FT120") != std::string::npos || pn_up.find("C1X") != std::string::npos) {
        return "Hesai FT120";
    }
    if (sn_up.find("FTX") != std::string::npos || pn_up.find("FTX") != std::string::npos) {
        return "Hesai FTX";
    }
    if (sn_up.find("OT128") != std::string::npos || pn_up.find("OT128") != std::string::npos) {
        return "Hesai OT128";
    }
    if (sn_up.find("JT128") != std::string::npos || pn_up.find("JT128") != std::string::npos) {
        return "Hesai JT128";
    }
    if (sn_up.find("XT32") != std::string::npos || pn_up.find("XT32") != std::string::npos) {
        return "Hesai XT32";
    }
    if (sn_up.find("AT128") != std::string::npos || pn_up.find("AT128") != std::string::npos || pn_up.find("E2X") != std::string::npos) {
        return "Hesai AT128";
    }
    if (sn_up.find("PANDAR40") != std::string::npos || sn_up.find("PA40") != std::string::npos) {
        return "Hesai Pandar40";
    }
    if (sn_up.find("PANDAR64") != std::string::npos || sn_up.find("PA64") != std::string::npos) {
        return "Hesai Pandar64";
    }
    if (sn_up.find("PANDAR128") != std::string::npos || sn_up.find("PA128") != std::string::npos) {
        return "Hesai Pandar128";
    }
    if (sn_up.find("QT") != std::string::npos || pn_up.find("QT") != std::string::npos) {
        return "Hesai PandarQT";
    }
    return !pn.empty() ? pn : "Hesai LiDAR";
}

std::string HesaiPtcClient::sanitize_ip(const std::string& input) {
    std::string clean = input;
    size_t space_pos = clean.find(' ');
    if (space_pos != std::string::npos) {
        clean = clean.substr(0, space_pos);
    }
    if (clean.rfind("http://", 0) == 0) {
        clean = clean.substr(7);
    } else if (clean.rfind("https://", 0) == 0) {
        clean = clean.substr(8);
    }
    size_t colon_pos = clean.find(':');
    if (colon_pos != std::string::npos) {
        clean = clean.substr(0, colon_pos);
    }
    size_t slash_pos = clean.find('/');
    if (slash_pos != std::string::npos) {
        clean = clean.substr(0, slash_pos);
    }
    return clean;
}

std::string HesaiPtcClient::extract_field(const std::string& text, const std::string& key) {
    if (text.empty() || key.empty()) return "";

    auto to_lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };

    std::string key_quoted = "\"" + key + "\"";
    std::string lower_key_quoted = key_quoted;
    std::transform(lower_key_quoted.begin(), lower_key_quoted.end(), lower_key_quoted.begin(), to_lower);

    // Search case-insensitively for "key"
    size_t pos = std::string::npos;
    for (size_t i = 0; i + lower_key_quoted.size() <= text.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < lower_key_quoted.size(); ++j) {
            if (to_lower(text[i + j]) != lower_key_quoted[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            pos = i;
            break;
        }
    }

    if (pos == std::string::npos) {
        // Fallback: search key without quotes (HTML pattern like "Serial Number: XYZ")
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), to_lower);
        for (size_t i = 0; i + lower_key.size() <= text.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < lower_key.size(); ++j) {
                if (to_lower(text[i + j]) != lower_key[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                size_t p = i + lower_key.size();
                while (p < text.size() && (text[p] == ' ' || text[p] == ':' || text[p] == '=' || text[p] == '\t')) p++;
                size_t end_p = p;
                while (end_p < text.size() && text[end_p] != '<' && text[end_p] != '\r' && text[end_p] != '\n' && text[end_p] != ',') {
                    end_p++;
                }
                return text.substr(p, end_p - p);
            }
        }
        return "";
    }

    // Locate colon
    size_t colon = text.find(':', pos + key_quoted.size());
    if (colon == std::string::npos || colon > pos + key_quoted.size() + 20) return "";

    // Skip whitespace
    size_t val_start = colon + 1;
    while (val_start < text.size() && (text[val_start] == ' ' || text[val_start] == '\t' || text[val_start] == '\r' || text[val_start] == '\n')) {
        val_start++;
    }
    if (val_start >= text.size()) return "";

    if (text[val_start] == '\"') {
        val_start++;
        size_t val_end = val_start;
        while (val_end < text.size()) {
            if (text[val_end] == '\"' && text[val_end - 1] != '\\') break;
            val_end++;
        }
        return text.substr(val_start, val_end - val_start);
    } else {
        size_t val_end = val_start;
        while (val_end < text.size() &&
               text[val_end] != ',' && text[val_end] != '}' && text[val_end] != ']' &&
               text[val_end] != '\r' && text[val_end] != '\n' && text[val_end] != ' ' && text[val_end] != '\t') {
            val_end++;
        }
        return text.substr(val_start, val_end - val_start);
    }
}

std::vector<uint8_t> HesaiPtcClient::build_ptc1_request(uint8_t cmd, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buf(8 + payload.size());
    buf[0] = PTC1_MAGIC[0];
    buf[1] = PTC1_MAGIC[1];
    buf[2] = cmd;
    buf[3] = 0x00; // sub-command / reserved
    write_be32(&buf[4], static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(&buf[8], payload.data(), payload.size());
    }
    return buf;
}

std::vector<uint8_t> HesaiPtcClient::build_ptc1_response(uint8_t cmd, uint8_t ret_code, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buf(8 + payload.size());
    buf[0] = PTC1_MAGIC[0];
    buf[1] = PTC1_MAGIC[1];
    buf[2] = cmd;
    buf[3] = ret_code;
    write_be32(&buf[4], static_cast<uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(&buf[8], payload.data(), payload.size());
    }
    return buf;
}

bool HesaiPtcClient::parse_inventory_payload(const uint8_t* data, size_t len, LidarDiagnosticParams& out) {
    if (len < 18) return false;

    // Detect OT128 layout: SN is followed by Date at 18..34, MAC at 34..40, PN at 109..141
    std::string cand_sn = extract_str(data, 18);
    bool is_ot128 = (cand_sn.rfind("OT", 0) == 0) ||
                    (len >= 141 && extract_str(data + 109, 32).find("OT128") != std::string::npos);

    if (is_ot128) {
        out.serial_number = cand_sn;
        if (len >= 34) out.date_of_manufacture = extract_str(data + 18, 16);
        if (len >= 40) out.mac_address = format_mac(data + 34);
        std::string sw_ver = (len >= 56) ? extract_str(data + 40, 16) : "";
        out.hardware_version = (len >= 72) ? extract_str(data + 56, 16) : "";
        out.controller_fw_version = (len >= 88) ? extract_str(data + 72, 16) : "";
        out.product_number = (len >= 141) ? extract_str(data + 109, 32) : "";

        if (!sw_ver.empty() && !out.controller_fw_version.empty()) {
            out.firmware_version = "SW: " + sw_ver + " / FW: " + out.controller_fw_version;
        } else if (!sw_ver.empty()) {
            out.firmware_version = sw_ver;
        } else {
            out.firmware_version = out.controller_fw_version;
        }
        out.model = synthesize_model_name(out.serial_number, out.product_number);
        return true;
    }

    // Standard Hesai layout:
    // 0..17: SN (18B)
    // 18..49: PN (32B)
    // 50..65: Date (16B)
    // 66..71: MAC (6B)
    // 72..87: SW Version (16B)
    // 88..103: HW Version (16B)
    // 104..119: Controller FW Version (16B)
    out.serial_number = cand_sn;
    if (len >= 50) out.product_number = extract_str(data + 18, 32);
    if (len >= 66) out.date_of_manufacture = extract_str(data + 50, 16);
    if (len >= 72) out.mac_address = format_mac(data + 66);
    std::string sw_ver = (len >= 88) ? extract_str(data + 72, 16) : "";
    if (len >= 104) out.hardware_version = extract_str(data + 88, 16);
    if (len >= 120) out.controller_fw_version = extract_str(data + 104, 16);

    if (!sw_ver.empty() && !out.controller_fw_version.empty()) {
        out.firmware_version = "SW: " + sw_ver + " / FW: " + out.controller_fw_version;
    } else if (!sw_ver.empty()) {
        out.firmware_version = sw_ver;
    } else {
        out.firmware_version = out.controller_fw_version;
    }

    out.model = synthesize_model_name(out.serial_number, out.product_number);
    return true;
}

bool HesaiPtcClient::parse_status_payload(const uint8_t* data, size_t len, LidarDiagnosticParams& out) {
    if (len < 10) return false;

    out.uptime_s = read_be32(data);
    out.motor_rpm = static_cast<float>(read_be16(data + 4));

    // Determine number of temperature channels (8 for OT128, 9 for standard)
    bool is_ot128 = (out.model.find("OT128") != std::string::npos);
    size_t num_temp_channels = is_ot128 ? 8 : 9;

    out.temperatures_c.clear();
    for (size_t i = 0; i < num_temp_channels; ++i) {
        size_t off = 6 + i * 4;
        if (off + 4 <= len) {
            int32_t raw_t = read_be32_signed(data + off);
            float t_c = static_cast<float>(raw_t) / 100.0f;
            out.temperatures_c.push_back(t_c);
        }
    }

    if (!out.temperatures_c.empty()) {
        out.temperature_c = out.temperatures_c[0]; // Representative primary channel
    }

    if (is_ot128) {
        if (len >= 44) out.startup_times = read_be32(data + 40);
        if (len >= 48) out.total_operation_time_s = read_be32(data + 44);
        if (len > 48) out.ptp_status = data[48];
        if (len >= 53) {
            int32_t raw_h = read_be32_signed(data + 49);
            out.humidity_percent = static_cast<float>(raw_h) / 10.0f;
        }
    } else {
        if (len >= 48) out.startup_times = read_be32(data + 44);
        if (len >= 52) out.total_operation_time_s = read_be32(data + 48);
        if (len > 52) out.ptp_status = data[52];
        if (len >= 57) {
            uint32_t raw_h = read_be32(data + 53);
            if (raw_h > 0 && raw_h < 1000) {
                out.humidity_percent = static_cast<float>(raw_h) / 10.0f;
            }
        }
    }

    // Flash pure solid state LiDARs have 0 RPM
    if (out.model.find("FT120") != std::string::npos || out.model.find("FTX") != std::string::npos) {
        out.motor_rpm = 0.0f;
    }

    return true;
}

bool HesaiPtcClient::parse_monitor_payload(const uint8_t* data, size_t len, LidarDiagnosticParams& out) {
    if (len < 12) return false;
    out.operating_current_ma = static_cast<float>(read_be32_signed(data)) / 100.0f;
    out.operating_voltage    = static_cast<float>(read_be32_signed(data + 4)) / 100.0f;
    out.operating_power_w    = static_cast<float>(read_be32_signed(data + 8)) / 100.0f;
    return true;
}

bool HesaiPtcClient::parse_config_payload(const uint8_t* data, size_t len, LidarDiagnosticParams& out) {
    if (len < 18) return false;
    out.ip_address = format_ip(data);
    out.subnet_mask = format_ip(data + 4);
    out.gateway = format_ip(data + 8);
    out.destination_ip = format_ip(data + 12);
    out.destination_port = read_be16(data + 16);
    return true;
}

LidarDiagnosticParams HesaiPtcClient::query_http_fallback(const std::string& raw_ip, uint16_t port, int timeout_ms) {
    LidarDiagnosticParams res;
    std::string ip = sanitize_ip(raw_ip);
    res.ip_address = ip;

    TcpSocket sock;
    if (!sock.connect(ip, port, timeout_ms)) {
        res.success = false;
        res.error_msg = "HTTP port " + std::to_string(port) + " connection timed out or refused";
        return res;
    }

    // 1. Fetch device_info
    std::string req = "GET /pandar.cgi?action=get&object=device_info HTTP/1.1\r\n"
                      "Host: " + ip + "\r\n"
                      "User-Agent: UniversalLiDARViewer/2.0\r\n"
                      "Accept: application/json, text/html, */*\r\n"
                      "Connection: close\r\n\r\n";

    if (!sock.send_exact(req.data(), req.size(), timeout_ms)) {
        res.success = false;
        res.error_msg = "Failed to send HTTP GET request to port " + std::to_string(port);
        return res;
    }

    std::string resp_body;
    if (!sock.recv_all_until_close(resp_body, timeout_ms)) {
        res.success = false;
        res.error_msg = "Empty or timed out HTTP response from port " + std::to_string(port);
        return res;
    }

    // Extract core fields
    std::string sn = extract_field(resp_body, "SN");
    if (sn.empty()) sn = extract_field(resp_body, "sn");
    if (sn.empty()) sn = extract_field(resp_body, "serial_number");

    std::string prod = extract_field(resp_body, "ProdName");
    std::string model = extract_field(resp_body, "Model");
    std::string sw_ver = extract_field(resp_body, "SW_Ver");
    std::string fw_ver = extract_field(resp_body, "FW_Ver");
    std::string mac = extract_field(resp_body, "Mac");
    std::string date = extract_field(resp_body, "ProdDate");
    std::string speed = extract_field(resp_body, "SpinSpeed");
    std::string temp_str = extract_field(resp_body, "CurrentTemp");

    if (sn.empty() && prod.empty() && model.empty()) {
        res.success = false;
        res.error_msg = "HTTP response received but contained no LiDAR identity fields";
        return res;
    }

    res.success = true;
    res.vendor = "Hesai";
    res.serial_number = sn;
    res.model = !prod.empty() ? prod : (!model.empty() ? model : "Hesai Pandar");
    res.mac_address = mac;
    res.date_of_manufacture = date;

    if (!fw_ver.empty() && !sw_ver.empty()) {
        res.firmware_version = "FW " + fw_ver + " / SW " + sw_ver;
    } else if (!sw_ver.empty()) {
        res.firmware_version = sw_ver;
    } else {
        res.firmware_version = fw_ver;
    }

    // Parse speed
    if (speed == "1") res.motor_rpm = 300.0f;
    else if (speed == "2") res.motor_rpm = 600.0f;
    else if (speed == "3") res.motor_rpm = 1200.0f;
    else if (!speed.empty()) {
        try { res.motor_rpm = std::stof(speed); } catch (...) {}
    }

    // Parse temperature
    if (!temp_str.empty()) {
        try { res.temperature_c = std::stof(temp_str); } catch (...) {}
    }

    // Secondary fetch: if temperature or speed were missing, attempt TimeStatistic and lidar_config
    if (res.temperature_c <= 0.0f) {
        TcpSocket sock2;
        if (sock2.connect(ip, port, std::min(timeout_ms, 500))) {
            std::string req2 = "GET /pandar.cgi?action=get&object=TimeStatistic HTTP/1.1\r\n"
                               "Host: " + ip + "\r\nConnection: close\r\n\r\n";
            if (sock2.send_exact(req2.data(), req2.size(), 500)) {
                std::string resp2;
                if (sock2.recv_all_until_close(resp2, 500)) {
                    std::string t2 = extract_field(resp2, "CurrentTemp");
                    if (!t2.empty()) {
                        try { res.temperature_c = std::stof(t2); } catch (...) {}
                    }
                }
            }
        }
    }

    if (res.motor_rpm <= 0.0f) {
        TcpSocket sock3;
        if (sock3.connect(ip, port, std::min(timeout_ms, 500))) {
            std::string req3 = "GET /pandar.cgi?action=get&object=lidar_config HTTP/1.1\r\n"
                               "Host: " + ip + "\r\nConnection: close\r\n\r\n";
            if (sock3.send_exact(req3.data(), req3.size(), 500)) {
                std::string resp3;
                if (sock3.recv_all_until_close(resp3, 500)) {
                    std::string sp = extract_field(resp3, "SpinSpeed");
                    if (sp == "1") res.motor_rpm = 300.0f;
                    else if (sp == "2") res.motor_rpm = 600.0f;
                    else if (sp == "3") res.motor_rpm = 1200.0f;
                    else if (!sp.empty()) {
                        try { res.motor_rpm = std::stof(sp); } catch (...) {}
                    }
                }
            }
        }
    }

    return res;
}

LidarDiagnosticParams HesaiPtcClient::query_ptc(const std::string& raw_ip, uint16_t port, int timeout_ms) {
    LidarDiagnosticParams res;
    std::string ip = sanitize_ip(raw_ip);
    res.ip_address = ip;
    res.vendor = "Hesai";

    TcpSocket sock;
    if (!sock.connect(ip, port, timeout_ms)) {
        res.success = false;
        res.error_msg = "PTC TCP port " + std::to_string(port) + " connection timed out or refused";
        return res;
    }

    // Helper lambda to send a PTC command and receive the response (handling PTC 1.0 & PTC 2.0)
    auto execute_cmd = [&](uint8_t cmd, const std::vector<uint8_t>& payload,
                           uint8_t& out_ret_code, std::vector<uint8_t>& out_payload) -> bool {
        auto req = build_ptc1_request(cmd, payload);
        if (!sock.send_exact(req.data(), req.size(), timeout_ms)) {
            return false;
        }

        // Read first 2 bytes to distinguish PTC 1.0 ("Gt") vs PTC 2.0 ("Wu")
        uint8_t magic[2];
        if (!sock.recv_exact(magic, 2, timeout_ms)) {
            return false;
        }

        uint8_t resp_cmd = 0;
        uint32_t payload_len = 0;

        if (magic[0] == PTC1_MAGIC[0] && magic[1] == PTC1_MAGIC[1]) {
            // PTC 1.0: 6 remaining bytes: cmd(1B), ret_code(1B), payload_len(4B)
            uint8_t hdr_rem[6];
            if (!sock.recv_exact(hdr_rem, 6, timeout_ms)) {
                return false;
            }
            resp_cmd = hdr_rem[0];
            out_ret_code = hdr_rem[1];
            payload_len = read_be32(&hdr_rem[2]);
        } else if (magic[0] == PTC2_MAGIC[0] && magic[1] == PTC2_MAGIC[1]) {
            // PTC 2.0: 10 remaining bytes:
            // ver(1B) + cmd(1B) + ret_code(1B) + rsv(1B) + check_type(1B) + check_len(1B) + payload_len(4B)
            uint8_t hdr_rem[10];
            if (!sock.recv_exact(hdr_rem, 10, timeout_ms)) {
                return false;
            }
            resp_cmd = hdr_rem[1];
            out_ret_code = hdr_rem[2];
            payload_len = read_be32(&hdr_rem[6]);
        } else {
            return false; // Unknown magic identifier
        }

        if (resp_cmd != cmd) {
            return false; // Command mismatch
        }

        // Safety limit: 8MB threshold to protect against malformed frames
        if (payload_len > 8 * 1024 * 1024) {
            return false;
        }

        out_payload.resize(payload_len);
        if (payload_len > 0) {
            if (!sock.recv_exact(out_payload.data(), payload_len, timeout_ms)) {
                return false;
            }
        }
        return true;
    };

    // 1. Mandatory: Command 0x07 (Inventory Info)
    uint8_t ret_code = 0;
    std::vector<uint8_t> inv_payload;
    if (!execute_cmd(CMD_GET_INVENTORY_INFO, {}, ret_code, inv_payload) || ret_code != 0) {
        res.success = false;
        res.error_msg = "PTC 0x07 (Inventory) query failed with code " + std::to_string(ret_code);
        return res;
    }
    if (!parse_inventory_payload(inv_payload.data(), inv_payload.size(), res)) {
        res.success = false;
        res.error_msg = "PTC 0x07 (Inventory) returned malformed payload";
        return res;
    }

    // 2. Command 0x09 (Lidar Status: Uptime, RPM, Temps)
    std::vector<uint8_t> status_payload;
    if (execute_cmd(CMD_GET_LIDAR_STATUS, {}, ret_code, status_payload) && ret_code == 0) {
        parse_status_payload(status_payload.data(), status_payload.size(), res);
    }

    // 3. Command 0x27 (Lidar Monitor: Voltage, Current, Power)
    // Note: On pure solid-state (e.g. FT120/FTX), Command 0x27 is not supported and returns error code.
    // Graceful degradation: do NOT fail the overall query.
    std::vector<uint8_t> mon_payload;
    if (execute_cmd(CMD_GET_LIDAR_MONITOR, {}, ret_code, mon_payload) && ret_code == 0) {
        parse_monitor_payload(mon_payload.data(), mon_payload.size(), res);
    }

    // 4. Command 0x08 (Config Info: Network settings)
    std::vector<uint8_t> cfg_payload;
    if (execute_cmd(CMD_GET_CONFIG_INFO, {}, ret_code, cfg_payload) && ret_code == 0) {
        parse_config_payload(cfg_payload.data(), cfg_payload.size(), res);
    }

    res.success = true;
    return res;
}

LidarDiagnosticParams HesaiPtcClient::query(const std::string& raw_ip, uint16_t port, int timeout_ms) {
    std::string ip = sanitize_ip(raw_ip);
    if (ip.empty()) {
        LidarDiagnosticParams res;
        res.success = false;
        res.error_msg = "Invalid or empty IP address";
        return res;
    }

    if (port == 80) {
        return query_http_fallback(ip, 80, timeout_ms);
    }

    // 1. Try PTC 9347
    LidarDiagnosticParams ptc_res = query_ptc(ip, port, timeout_ms);
    if (ptc_res.success && !ptc_res.serial_number.empty()) {
        return ptc_res;
    }

    // 2. Fallback to HTTP 80
    LidarDiagnosticParams http_res = query_http_fallback(ip, 80, timeout_ms);
    if (http_res.success && !http_res.serial_number.empty()) {
        return http_res;
    }

    // 3. Return combined error details
    LidarDiagnosticParams failed;
    failed.success = false;
    failed.ip_address = ip;
    failed.error_msg = "PTC 9347 (" + (ptc_res.error_msg.empty() ? "无响应" : ptc_res.error_msg) + 
                       ") 且 HTTP 80 回退 (" + (http_res.error_msg.empty() ? "无响应" : http_res.error_msg) + ") 均失败";
    return failed;
}

} // namespace ulv
