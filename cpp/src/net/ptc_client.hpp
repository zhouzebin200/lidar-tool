#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace ulv {

/**
 * @brief LiDAR diagnostic parameters retrieved from PTC TCP 9347 or HTTP port 80.
 * Conforms to PROJECT.md §Interface Contracts M3 and supports rich telemetry fields.
 */
struct LidarDiagnosticParams {
    bool success = false;
    std::string error_msg;

    // Identification & Version Info (Command 0x07 Inventory / HTTP device_info)
    std::string vendor = "Hesai";
    std::string model;
    std::string serial_number;
    std::string product_number;
    std::string date_of_manufacture;
    std::string mac_address;
    std::string firmware_version;
    std::string hardware_version;
    std::string controller_fw_version;

    // Network Configuration (Command 0x08 Config)
    std::string ip_address;
    std::string subnet_mask;
    std::string gateway;
    std::string destination_ip;
    uint16_t destination_port = 0;

    // Operating & Status Telemetry (Command 0x09 Status / HTTP TimeStatistic)
    uint32_t uptime_s = 0;
    uint32_t startup_times = 0;
    uint32_t total_operation_time_s = 0;
    float motor_rpm = 0.0f;
    float temperature_c = 0.0f;            // Primary / board temperature
    std::vector<float> temperatures_c;     // Multi-channel temperature sensors
    float humidity_percent = 0.0f;         // Internal relative humidity (if supported)
    uint8_t ptp_status = 0;                // 0: Free run, 1: Tracking, 2: Locked, 3: Frozen

    // Electrical Monitoring Telemetry (Command 0x27 Monitor)
    float operating_voltage = 0.0f;        // Input Voltage in Volts (V)
    float operating_current_ma = 0.0f;     // Input Current in Milliamperes (mA)
    float operating_power_w = 0.0f;        // Input Power in Watts (W)
};

/**
 * @brief Pure C++17 Hesai PTC 9347 protocol client with automatic HTTP fallback.
 */
class HesaiPtcClient {
public:
    // Standard PTC Magic Identifiers
    static constexpr uint8_t PTC1_MAGIC[2] = {0x47, 0x74}; // "Gt"
    static constexpr uint8_t PTC2_MAGIC[2] = {0x57, 0x75}; // "Wu"

    // Standard PTC Command Codes
    static constexpr uint8_t CMD_GET_INVENTORY_INFO = 0x07;
    static constexpr uint8_t CMD_GET_CONFIG_INFO    = 0x08;
    static constexpr uint8_t CMD_GET_LIDAR_STATUS   = 0x09;
    static constexpr uint8_t CMD_GET_LIDAR_MONITOR  = 0x27;

    /**
     * @brief Queries LiDAR parameters. First attempts TCP port 9347 (PTC).
     * If port 9347 fails or is refused, automatically falls back to HTTP port 80.
     * @param ip Target LiDAR IP address (e.g. "192.168.1.201").
     * @param port Target port (default: 9347). If set to 80, directly queries HTTP.
     * @param timeout_ms Maximum socket connection and transfer timeout in milliseconds (default: 1500).
     */
    static LidarDiagnosticParams query(const std::string& ip, uint16_t port = 9347, int timeout_ms = 1500);

    /**
     * @brief Queries LiDAR parameters specifically via PTC binary protocol (port 9347).
     */
    static LidarDiagnosticParams query_ptc(const std::string& ip, uint16_t port = 9347, int timeout_ms = 1500);

    /**
     * @brief Queries LiDAR parameters specifically via HTTP web management interface (port 80).
     */
    static LidarDiagnosticParams query_http_fallback(const std::string& ip, uint16_t port = 80, int timeout_ms = 1500);

    // Protocol helper methods
    static std::vector<uint8_t> build_ptc1_request(uint8_t cmd, const std::vector<uint8_t>& payload = {});
    static std::vector<uint8_t> build_ptc1_response(uint8_t cmd, uint8_t ret_code, const std::vector<uint8_t>& payload = {});

    static bool parse_inventory_payload(const uint8_t* data, size_t len, LidarDiagnosticParams& out);
    static bool parse_status_payload(const uint8_t* data, size_t len, LidarDiagnosticParams& out);
    static bool parse_monitor_payload(const uint8_t* data, size_t len, LidarDiagnosticParams& out);
    static bool parse_config_payload(const uint8_t* data, size_t len, LidarDiagnosticParams& out);

    /**
     * @brief Extracts JSON or key-value fields from HTTP responses without external dependencies.
     */
    static std::string extract_field(const std::string& text, const std::string& key);

    /**
     * @brief Cleans up input IP address strings (strips "http://", port suffixes, or annotations).
     */
    static std::string sanitize_ip(const std::string& input);

    /**
     * @brief Synthesizes human-readable LiDAR model from SN and PN.
     */
    static std::string synthesize_model_name(const std::string& sn, const std::string& pn);
};

} // namespace ulv
