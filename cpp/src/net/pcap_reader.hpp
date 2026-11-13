#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <functional>

namespace ulv {

struct PcapUdpPacket {
    uint64_t timestamp_ns = 0;
    std::string src_ip;
    std::string dst_ip;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    uint16_t vlan_id = 0;
    std::vector<uint8_t> payload;
};

class PcapReader {
public:
    PcapReader();
    ~PcapReader();

    bool open(const std::string& filepath);
    void close();

    bool is_open() const { return is_open_; }
    size_t total_packets() const { return packet_offsets_.size(); }
    const std::string& filepath() const { return filepath_; }

    // Read next valid UDP packet in sequence
    bool read_next_packet(PcapUdpPacket& out_packet);

    // Rewind to beginning
    void rewind();

    // Seek to packet index
    bool seek_to_index(size_t index);

    // Current packet index
    size_t current_index() const { return current_index_; }

private:
    std::string filepath_;
    std::ifstream file_;
    bool is_open_ = false;
    bool is_pcapng_ = false;
    bool swap_byte_order_ = false;
    bool is_nanosec_ = false;
    uint8_t ts_resol_ = 6;
    size_t current_index_ = 0;

    std::vector<uint64_t> packet_offsets_;

    bool parse_packet_at_offset(uint64_t offset, PcapUdpPacket& out_packet, uint64_t* next_offset);
    bool parse_ethernet_ip_udp(const uint8_t* buf, size_t caplen, uint64_t ts_ns, PcapUdpPacket& out);
};

} // namespace ulv
