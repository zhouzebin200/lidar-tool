#include "pcap_reader.hpp"
#include <filesystem>
#include <iostream>
#include <cstring>

namespace ulv {

#pragma pack(push, 1)
struct PcapGlobalHeader {
    uint32_t magic_number;   // 0xa1b2c3d4 or 0xd4c3b2a1
    uint16_t version_major;  // 2
    uint16_t version_minor;  // 4
    int32_t  thiszone;       // GMT to local correction
    uint32_t sigfigs;        // accuracy of timestamps
    uint32_t snaplen;        // max length of captured packets
    uint32_t network;        // data link type (1 = DLT_EN10MB)
};

struct PcapRecordHeader {
    uint32_t ts_sec;         // timestamp seconds
    uint32_t ts_usec;        // timestamp microseconds (or nanoseconds)
    uint32_t incl_len;       // number of octets of packet saved in file
    uint32_t orig_len;       // actual length of packet
};
#pragma pack(pop)

static uint16_t swap16(uint16_t val) {
    return (val << 8) | (val >> 8);
}

static uint32_t swap32(uint32_t val) {
    return ((val >> 24) & 0xff) |
           ((val << 8) & 0xff0000) |
           ((val >> 8) & 0xff00) |
           ((val << 24) & 0xff000000);
}

PcapReader::PcapReader() = default;

PcapReader::~PcapReader() {
    close();
}

bool PcapReader::open(const std::string& filepath) {
    close();
    filepath_ = filepath;
    file_.open(std::filesystem::u8path(filepath), std::ios::binary);
    if (!file_.is_open()) {
        std::cerr << "Failed to open PCAP file: " << filepath << std::endl;
        return false;
    }

    uint32_t magic = 0;
    if (!file_.read(reinterpret_cast<char*>(&magic), sizeof(magic))) {
        close();
        return false;
    }

    if (magic == 0x0a0d0d0a) {
        // PCAPNG format
        is_pcapng_ = true;
        uint32_t shb_len = 0;
        uint32_t bom = 0;
        if (!file_.read(reinterpret_cast<char*>(&shb_len), 4) ||
            !file_.read(reinterpret_cast<char*>(&bom), 4)) {
            close();
            return false;
        }

        if (bom == 0x1a2b3c4d) {
            swap_byte_order_ = false;
        } else if (bom == 0x4d3c2b1a) {
            swap_byte_order_ = true;
        } else {
            std::cerr << "Unsupported PCAPNG BOM: 0x" << std::hex << bom << std::dec << std::endl;
            close();
            return false;
        }

        file_.seekg(0, std::ios::end);
        uint64_t file_size = file_.tellg();
        uint64_t current_offset = 0;
        packet_offsets_.clear();
        ts_resol_ = 6; // default microsecond

        while (current_offset + 8 <= file_size) {
            file_.seekg(current_offset);
            uint32_t btype = 0, blen = 0;
            if (!file_.read(reinterpret_cast<char*>(&btype), 4) ||
                !file_.read(reinterpret_cast<char*>(&blen), 4)) {
                break;
            }
            if (swap_byte_order_) {
                btype = swap32(btype);
                blen = swap32(blen);
            }
            if (blen < 12 || current_offset + blen > file_size) {
                break;
            }

            if (btype == 0x00000001) { // Interface Description Block
                std::vector<uint8_t> idb(blen - 8);
                if (file_.read(reinterpret_cast<char*>(idb.data()), blen - 8)) {
                    size_t pos = 8; // skip link_type(2) + reserved(2) + snaplen(4)
                    while (pos + 4 <= idb.size()) {
                        uint16_t opt_code = idb[pos] | (idb[pos + 1] << 8);
                        uint16_t opt_len = idb[pos + 2] | (idb[pos + 3] << 8);
                        if (swap_byte_order_) {
                            opt_code = swap16(opt_code);
                            opt_len = swap16(opt_len);
                        }
                        pos += 4;
                        if (opt_code == 0) break;
                        if (opt_code == 9 && opt_len >= 1 && pos < idb.size()) {
                            ts_resol_ = idb[pos];
                        }
                        pos += (opt_len + 3) & ~3;
                    }
                }
            } else if (btype == 0x00000006 || btype == 0x00000002) { // EPB or SPB
                packet_offsets_.push_back(current_offset);
            }
            current_offset += blen;
        }

        is_open_ = true;
        current_index_ = 0;
        file_.clear();
        if (!packet_offsets_.empty()) {
            file_.seekg(packet_offsets_[0]);
        }
        std::cout << "Opened PCAPNG: " << filepath << " with " << packet_offsets_.size() << " packets." << std::endl;
        return true;
    }

    // Classic PCAP format
    is_pcapng_ = false;
    file_.seekg(0);
    PcapGlobalHeader gh{};
    if (!file_.read(reinterpret_cast<char*>(&gh), sizeof(gh))) {
        close();
        return false;
    }

    if (gh.magic_number == 0xa1b2c3d4) {
        swap_byte_order_ = false;
        is_nanosec_ = false;
    } else if (gh.magic_number == 0xd4c3b2a1) {
        swap_byte_order_ = true;
        is_nanosec_ = false;
    } else if (gh.magic_number == 0xa1b23c4d) {
        swap_byte_order_ = false;
        is_nanosec_ = true;
    } else if (gh.magic_number == 0x4d3cb2a1) {
        swap_byte_order_ = true;
        is_nanosec_ = true;
    } else {
        std::cerr << "Unsupported PCAP magic: 0x" << std::hex << gh.magic_number << std::dec << std::endl;
        close();
        return false;
    }

    uint32_t link_type = swap_byte_order_ ? swap32(gh.network) : gh.network;
    if (link_type != 1) { // 1 = DLT_EN10MB (Ethernet)
        std::cerr << "Unsupported Link Type: " << link_type << " (expected 1 for Ethernet)" << std::endl;
        close();
        return false;
    }

    // Build packet index
    packet_offsets_.clear();
    uint64_t current_offset = sizeof(PcapGlobalHeader);

    while (true) {
        file_.seekg(current_offset);
        PcapRecordHeader rh{};
        if (!file_.read(reinterpret_cast<char*>(&rh), sizeof(rh))) {
            break;
        }

        uint32_t caplen = swap_byte_order_ ? swap32(rh.incl_len) : rh.incl_len;
        if (caplen == 0 || caplen > 65536) {
            break;
        }

        packet_offsets_.push_back(current_offset);
        current_offset += sizeof(PcapRecordHeader) + caplen;
    }

    is_open_ = true;
    current_index_ = 0;
    file_.clear();
    file_.seekg(sizeof(PcapGlobalHeader));

    std::cout << "Opened PCAP: " << filepath << " with " << packet_offsets_.size() << " packets." << std::endl;
    return true;
}

void PcapReader::close() {
    if (file_.is_open()) {
        file_.close();
    }
    is_open_ = false;
    is_pcapng_ = false;
    packet_offsets_.clear();
    current_index_ = 0;
}

void PcapReader::rewind() {
    current_index_ = 0;
    if (file_.is_open()) {
        file_.clear();
        if (is_pcapng_) {
            if (!packet_offsets_.empty()) {
                file_.seekg(packet_offsets_[0]);
            }
        } else {
            file_.seekg(sizeof(PcapGlobalHeader));
        }
    }
}

bool PcapReader::seek_to_index(size_t index) {
    if (index >= packet_offsets_.size() || !file_.is_open()) {
        return false;
    }
    current_index_ = index;
    file_.clear();
    file_.seekg(packet_offsets_[index]);
    return true;
}

bool PcapReader::read_next_packet(PcapUdpPacket& out_packet) {
    if (!is_open_) return false;

    while (current_index_ < packet_offsets_.size()) {
        uint64_t offset = packet_offsets_[current_index_];
        current_index_++;

        uint64_t dummy = 0;
        if (parse_packet_at_offset(offset, out_packet, &dummy)) {
            return true;
        }
    }

    return false;
}

bool PcapReader::parse_packet_at_offset(uint64_t offset, PcapUdpPacket& out, uint64_t* next_offset) {
    file_.clear();
    file_.seekg(offset);

    if (is_pcapng_) {
        uint32_t btype = 0, blen = 0;
        if (!file_.read(reinterpret_cast<char*>(&btype), 4) ||
            !file_.read(reinterpret_cast<char*>(&blen), 4)) return false;
        if (swap_byte_order_) {
            btype = swap32(btype);
            blen = swap32(blen);
        }
        if (next_offset) *next_offset = offset + blen;

        if (btype == 0x00000006) { // Enhanced Packet Block
            uint32_t iface_id = 0, ts_high = 0, ts_low = 0, caplen = 0, origlen = 0;
            file_.read(reinterpret_cast<char*>(&iface_id), 4);
            file_.read(reinterpret_cast<char*>(&ts_high), 4);
            file_.read(reinterpret_cast<char*>(&ts_low), 4);
            file_.read(reinterpret_cast<char*>(&caplen), 4);
            file_.read(reinterpret_cast<char*>(&origlen), 4);
            if (swap_byte_order_) {
                ts_high = swap32(ts_high);
                ts_low = swap32(ts_low);
                caplen = swap32(caplen);
            }
            if (caplen < 14 + 20 + 8 || caplen > blen) return false;

            std::vector<uint8_t> buf(caplen);
            if (!file_.read(reinterpret_cast<char*>(buf.data()), caplen)) return false;

            uint64_t ts_raw = (static_cast<uint64_t>(ts_high) << 32) | ts_low;
            uint64_t ts_ns = 0;
            if (ts_resol_ == 9) ts_ns = ts_raw;
            else if (ts_resol_ == 6) ts_ns = ts_raw * 1000ULL;
            else if (ts_resol_ == 3) ts_ns = ts_raw * 1000000ULL;
            else if (ts_resol_ < 9) {
                uint64_t mul = 1;
                for (int i = 0; i < (9 - ts_resol_); ++i) mul *= 10;
                ts_ns = ts_raw * mul;
            } else {
                ts_ns = ts_raw;
            }

            return parse_ethernet_ip_udp(buf.data(), caplen, ts_ns, out);
        }
        return false;
    }

    // Classic PCAP
    PcapRecordHeader rh{};
    if (!file_.read(reinterpret_cast<char*>(&rh), sizeof(rh))) return false;

    uint32_t ts_sec = swap_byte_order_ ? swap32(rh.ts_sec) : rh.ts_sec;
    uint32_t ts_usec = swap_byte_order_ ? swap32(rh.ts_usec) : rh.ts_usec;
    uint32_t caplen = swap_byte_order_ ? swap32(rh.incl_len) : rh.incl_len;

    if (next_offset) {
        *next_offset = offset + sizeof(PcapRecordHeader) + caplen;
    }

    if (caplen < 14 + 20 + 8) return false;

    std::vector<uint8_t> buf(caplen);
    if (!file_.read(reinterpret_cast<char*>(buf.data()), caplen)) return false;

    uint64_t ts_ns = is_nanosec_
        ? (static_cast<uint64_t>(ts_sec) * 1000000000ULL + ts_usec)
        : (static_cast<uint64_t>(ts_sec) * 1000000000ULL + ts_usec * 1000ULL);

    return parse_ethernet_ip_udp(buf.data(), caplen, ts_ns, out);
}

bool PcapReader::parse_ethernet_ip_udp(const uint8_t* buf, size_t caplen, uint64_t ts_ns, PcapUdpPacket& out) {
    if (!buf || caplen < 14 + 20 + 8) return false;

    size_t pos = 12; // Skip MAC addresses (6+6)
    uint16_t ethertype = (buf[pos] << 8) | buf[pos + 1];
    pos += 2;

    uint16_t vlan_id = 0;
    while (ethertype == 0x8100 || ethertype == 0x88a8) {
        if (pos + 4 > caplen) return false;
        vlan_id = ((buf[pos] & 0x0F) << 8) | buf[pos + 1];
        pos += 2;
        ethertype = (buf[pos] << 8) | buf[pos + 1];
        pos += 2;
    }

    if (ethertype != 0x0800) return false;

    if (pos + 20 > caplen) return false;
    size_t ip_start = pos;
    uint8_t ver_ihl = buf[ip_start];
    if ((ver_ihl >> 4) != 4) return false;
    size_t ihl = (ver_ihl & 0x0F) * 4;
    if (ihl < 20 || ip_start + ihl > caplen) return false;

    uint8_t protocol = buf[ip_start + 9];
    if (protocol != 17) return false;

    char src_ip[32], dst_ip[32];
    snprintf(src_ip, sizeof(src_ip), "%u.%u.%u.%u",
             buf[ip_start + 12], buf[ip_start + 13], buf[ip_start + 14], buf[ip_start + 15]);
    snprintf(dst_ip, sizeof(dst_ip), "%u.%u.%u.%u",
             buf[ip_start + 16], buf[ip_start + 17], buf[ip_start + 18], buf[ip_start + 19]);

    size_t udp_start = ip_start + ihl;
    if (udp_start + 8 > caplen) return false;

    uint16_t src_port = (buf[udp_start] << 8) | buf[udp_start + 1];
    uint16_t dst_port = (buf[udp_start + 2] << 8) | buf[udp_start + 3];
    uint16_t udp_len = (buf[udp_start + 4] << 8) | buf[udp_start + 5];

    if (udp_len < 8) return false;
    size_t payload_len = udp_len - 8;
    size_t payload_start = udp_start + 8;
    if (payload_start + payload_len > caplen) {
        payload_len = caplen - payload_start;
    }

    out.timestamp_ns = ts_ns;
    out.src_ip = src_ip;
    out.dst_ip = dst_ip;
    out.src_port = src_port;
    out.dst_port = dst_port;
    out.vlan_id = vlan_id;

    out.payload.assign(buf + payload_start, buf + payload_start + payload_len);
    return true;
}

} // namespace ulv
