#include "udp_receiver.hpp"
#include <iostream>
#include <cstring>
#include <chrono>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
using poll_size_t = ULONG;
using sock_ssize_t = int;
#define CLOSE_SOCKET(s) closesocket(s)
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
using poll_size_t = nfds_t;
using sock_ssize_t = ssize_t;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif
#define CLOSE_SOCKET(s) close(s)
#define POLL_FN poll
#endif

namespace ulv {

UdpReceiver::UdpReceiver() {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

UdpReceiver::~UdpReceiver() {
    stop();
#if defined(_WIN32)
    WSACleanup();
#endif
}

bool UdpReceiver::start(const std::string& bind_ip, const std::vector<uint16_t>& ports, UdpPacketCallback callback) {
    stop();
    if (ports.empty() || !callback) return false;

    packets_received_.store(0);
    bytes_received_.store(0);
    is_running_.store(true);

    std::promise<bool> bind_promise;
    std::future<bool> bind_future = bind_promise.get_future();
    worker_thread_ = std::thread(&UdpReceiver::run_worker, this, bind_ip, ports, callback, std::move(bind_promise));
    if (!bind_future.get()) {
        stop();
        return false;
    }
    return true;
}

void UdpReceiver::stop() {
    is_running_.store(false);
    if (worker_thread_.joinable()) {
        if (worker_thread_.get_id() != std::this_thread::get_id()) {
            worker_thread_.join();
        }
    }
}

void UdpReceiver::run_worker(const std::string& bind_ip, const std::vector<uint16_t>& ports, UdpPacketCallback callback, std::promise<bool> bind_promise) {
    std::vector<socket_t> sockets;
    std::vector<uint16_t> bound_ports;
    std::vector<struct pollfd> poll_fds;

    for (uint16_t port : ports) {
        socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd == INVALID_SOCKET) {
            std::cerr << "[UdpReceiver] Failed to create UDP socket for port " << port << std::endl;
            continue;
        }

        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
        int bcast = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

        // Increase receive buffer to 4MB for high data rate LiDAR
        int rcv_buf = 4 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv_buf), sizeof(rcv_buf));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        // Sanitize bind IP string (strip UI preset annotations like "0.0.0.0 (全部网卡)")
        std::string ip_clean = bind_ip;
        size_t space_idx = ip_clean.find(' ');
        if (space_idx != std::string::npos) {
            ip_clean = ip_clean.substr(0, space_idx);
        }

        if (ip_clean.empty() || ip_clean == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            if (inet_pton(AF_INET, ip_clean.c_str(), &addr.sin_addr) <= 0) {
                addr.sin_addr.s_addr = INADDR_ANY;
            }
        }

        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "[UdpReceiver] Failed to bind UDP socket to port " << port << std::endl;
            CLOSE_SOCKET(fd);
            continue;
        }

#if defined(_WIN32)
        u_long nb_mode = 1;
        ioctlsocket(fd, FIONBIO, &nb_mode);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

        sockets.push_back(fd);
        bound_ports.push_back(port);

        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        poll_fds.push_back(pfd);
    }

    if (sockets.empty()) {
        std::cerr << "[UdpReceiver] No valid sockets bound." << std::endl;
        is_running_.store(false);
        bind_promise.set_value(false);
        return;
    }
    bind_promise.set_value(true);

    std::cout << "[UdpReceiver] Listening on " << bind_ip << " on " << sockets.size() << " ports." << std::endl;

    std::vector<uint8_t> buffer(65536);

    while (is_running_.load()) {
        int ret = POLL_FN(poll_fds.data(), static_cast<poll_size_t>(poll_fds.size()), 50); // 50ms timeout
        if (ret < 0) {
#if !defined(_WIN32)
            if (errno == EINTR) {
                continue;
            }
#endif
            break;
        }
        if (ret == 0) {
            continue;
        }

        for (size_t i = 0; i < poll_fds.size(); ++i) {
            if (poll_fds[i].revents & POLLIN) {
                for (int burst = 0; burst < 128 && is_running_.load(); ++burst) {
                    struct sockaddr_in sender_addr{};
                    socklen_t sender_len = sizeof(sender_addr);

                    sock_ssize_t n = recvfrom(poll_fds[i].fd, reinterpret_cast<char*>(buffer.data()),
                                              static_cast<int>(buffer.size()), 0,
                                              reinterpret_cast<struct sockaddr*>(&sender_addr), &sender_len);
                    if (n <= 0) {
                        break;
                    }
                    char src_ip[32] = {0};
                    inet_ntop(AF_INET, &sender_addr.sin_addr, src_ip, sizeof(src_ip));
                    uint16_t src_port = ntohs(sender_addr.sin_port);
                    uint16_t dst_port = bound_ports[i];

                    auto now = std::chrono::steady_clock::now().time_since_epoch();
                    uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

                    packets_received_.fetch_add(1, std::memory_order_relaxed);
                    bytes_received_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

                    callback(buffer.data(), static_cast<size_t>(n), src_ip, src_port, dst_port, now_ns);
                }
            }
        }
    }

    for (socket_t fd : sockets) {
        CLOSE_SOCKET(fd);
    }
    sockets.clear();
    bound_ports.clear();
    std::cout << "[UdpReceiver] Stopped listening." << std::endl;
}

} // namespace ulv
