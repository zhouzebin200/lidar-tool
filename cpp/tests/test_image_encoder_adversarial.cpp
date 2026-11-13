#include "test_framework.hpp"
#include "renderer/image_encoder.hpp"

#include <cstring>
#include <vector>
#include <random>
#include <chrono>
#include <cstdint>

namespace {

// IEEE 802.3 32-bit Cyclic Redundancy Check for PNG chunk verification
uint32_t compute_crc32(const uint8_t* buf, size_t len) {
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        c ^= buf[i];
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
        }
    }
    return ~c;
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

} // namespace

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Fuzzing: Zero, Negative & Null Dimensions") {
    // 1. Null buffer pointer handling
    auto png_null = ulv::compress_rgba_to_png(nullptr, 100, 100);
    ASSERT_TRUE(png_null.empty());

    auto rgb_null = ulv::compress_rgb_to_png(nullptr, 100, 100);
    ASSERT_TRUE(rgb_null.empty());

    auto uri_null = ulv::rgba_to_png_data_uri(nullptr, 100, 100);
    ASSERT_TRUE(uri_null.empty());

    // 2. Zero dimensions handling
    std::vector<uint8_t> dummy(64, 128);
    ASSERT_TRUE(ulv::compress_rgba_to_png(dummy.data(), 0, 0).empty());
    ASSERT_TRUE(ulv::compress_rgba_to_png(dummy.data(), 0, 10).empty());
    ASSERT_TRUE(ulv::compress_rgba_to_png(dummy.data(), 10, 0).empty());

    ASSERT_TRUE(ulv::compress_rgb_to_png(dummy.data(), 0, 0).empty());
    ASSERT_TRUE(ulv::compress_rgb_to_png(dummy.data(), 0, 10).empty());
    ASSERT_TRUE(ulv::compress_rgb_to_png(dummy.data(), 10, 0).empty());

    ASSERT_TRUE(ulv::rgba_to_png_data_uri(dummy.data(), 0, 0).empty());

    // 3. Negative dimensions handling
    ASSERT_TRUE(ulv::compress_rgba_to_png(dummy.data(), -1, 10).empty());
    ASSERT_TRUE(ulv::compress_rgba_to_png(dummy.data(), 10, -1).empty());
    ASSERT_TRUE(ulv::compress_rgba_to_png(dummy.data(), -100, -100).empty());

    ASSERT_TRUE(ulv::compress_rgb_to_png(dummy.data(), -5, 5).empty());
    ASSERT_TRUE(ulv::compress_rgb_to_png(dummy.data(), 5, -5).empty());
    ASSERT_TRUE(ulv::compress_rgb_to_png(dummy.data(), -50, -50).empty());

    ASSERT_TRUE(ulv::rgba_to_png_data_uri(dummy.data(), -10, 10).empty());

    // 4. In-place vertical flip with pathological inputs
    ulv::flip_image_vertical(nullptr, 10, 10, 4);      // Should safely return
    ulv::flip_image_vertical(dummy.data(), 0, 0, 4);   // Should safely return
    ulv::flip_image_vertical(dummy.data(), -1, 10, 4); // Should safely return
    ulv::flip_image_vertical(dummy.data(), 10, -1, 4); // Should safely return
    ulv::flip_image_vertical(dummy.data(), 10, 10, 0); // Should safely return
    ulv::flip_image_vertical(dummy.data(), 10, 10, -1);// Should safely return
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Fuzzing: Minimal Dimensions (1x1, 1x2, 2x1)") {
    // 1x1 RGBA
    uint8_t pixel_rgba[4] = { 255, 128, 64, 200 };
    auto png_1x1 = ulv::compress_rgba_to_png(pixel_rgba, 1, 1);
    ASSERT_GT(png_1x1.size(), 30);
    const uint8_t kPngMagic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    ASSERT_EQ(std::memcmp(png_1x1.data(), kPngMagic, 8), 0);
    ASSERT_EQ(read_be32(png_1x1.data() + 16), 1); // width
    ASSERT_EQ(read_be32(png_1x1.data() + 20), 1); // height
    ASSERT_EQ(png_1x1[24], 8);                     // bit depth
    ASSERT_EQ(png_1x1[25], 6);                     // color type RGBA

    // 1x1 RGB
    uint8_t pixel_rgb[3] = { 100, 150, 250 };
    auto png_1x1_rgb = ulv::compress_rgb_to_png(pixel_rgb, 1, 1);
    ASSERT_GT(png_1x1_rgb.size(), 30);
    ASSERT_EQ(std::memcmp(png_1x1_rgb.data(), kPngMagic, 8), 0);
    ASSERT_EQ(read_be32(png_1x1_rgb.data() + 16), 1);
    ASSERT_EQ(read_be32(png_1x1_rgb.data() + 20), 1);
    ASSERT_EQ(png_1x1_rgb[25], 2); // color type RGB

    // 1x2 and 2x1 RGBA
    uint8_t pixels_1x2[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    auto png_1x2 = ulv::compress_rgba_to_png(pixels_1x2, 1, 2);
    ASSERT_EQ(read_be32(png_1x2.data() + 16), 1);
    ASSERT_EQ(read_be32(png_1x2.data() + 20), 2);

    uint8_t pixels_2x1[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    auto png_2x1 = ulv::compress_rgba_to_png(pixels_2x1, 2, 1);
    ASSERT_EQ(read_be32(png_2x1.data() + 16), 2);
    ASSERT_EQ(read_be32(png_2x1.data() + 20), 1);
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Fuzzing: Extreme Aspect Ratios (16x1, 1x16, 1024x1, 1x1024)") {
    // 16x1 RGBA
    std::vector<uint8_t> wide_16x1(16 * 1 * 4, 180);
    auto png_16x1 = ulv::compress_rgba_to_png(wide_16x1.data(), 16, 1);
    ASSERT_EQ(read_be32(png_16x1.data() + 16), 16);
    ASSERT_EQ(read_be32(png_16x1.data() + 20), 1);

    // 1x16 RGBA
    std::vector<uint8_t> tall_1x16(1 * 16 * 4, 210);
    auto png_1x16 = ulv::compress_rgba_to_png(tall_1x16.data(), 1, 16);
    ASSERT_EQ(read_be32(png_1x16.data() + 16), 1);
    ASSERT_EQ(read_be32(png_1x16.data() + 20), 16);

    // 1024x1 RGBA
    std::vector<uint8_t> wide_1024(1024 * 1 * 4, 99);
    auto png_1024 = ulv::compress_rgba_to_png(wide_1024.data(), 1024, 1);
    ASSERT_EQ(read_be32(png_1024.data() + 16), 1024);
    ASSERT_EQ(read_be32(png_1024.data() + 20), 1);

    // 1x1024 RGBA
    std::vector<uint8_t> tall_1024(1 * 1024 * 4, 77);
    auto png_tall_1024 = ulv::compress_rgba_to_png(tall_1024.data(), 1, 1024);
    ASSERT_EQ(read_be32(png_tall_1024.data() + 16), 1);
    ASSERT_EQ(read_be32(png_tall_1024.data() + 20), 1024);

    // In-place vertical flip on 1x16
    std::vector<uint8_t> flip_test(16 * 4);
    for (int y = 0; y < 16; ++y) {
        flip_test[y * 4] = static_cast<uint8_t>(y);
    }
    ulv::flip_image_vertical(flip_test.data(), 1, 16, 4);
    for (int y = 0; y < 16; ++y) {
        ASSERT_EQ(flip_test[y * 4], static_cast<uint8_t>(15 - y));
    }
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Fuzzing: Non-Multiple-of-4 Positive Strides") {
    // 1. RGB 5x5: width = 5, channels = 3. Packed row = 15 bytes (non-multiple of 4)
    // Stride 15 (packed)
    std::vector<uint8_t> rgb_stride15(15 * 5, 120);
    auto png1 = ulv::compress_rgb_to_png(rgb_stride15.data(), 5, 5, 15);
    ASSERT_FALSE(png1.empty());
    ASSERT_EQ(read_be32(png1.data() + 16), 5);
    ASSERT_EQ(read_be32(png1.data() + 20), 5);

    // Stride 16 (4-byte aligned)
    std::vector<uint8_t> rgb_stride16(16 * 5, 130);
    auto png2 = ulv::compress_rgb_to_png(rgb_stride16.data(), 5, 5, 16);
    ASSERT_FALSE(png2.empty());

    // Stride 17 (prime number)
    std::vector<uint8_t> rgb_stride17(17 * 5, 140);
    auto png3 = ulv::compress_rgb_to_png(rgb_stride17.data(), 5, 5, 17);
    ASSERT_FALSE(png3.empty());

    // Stride 23 (unaligned padded)
    std::vector<uint8_t> rgb_stride23(23 * 5, 150);
    auto png4 = ulv::compress_rgb_to_png(rgb_stride23.data(), 5, 5, 23);
    ASSERT_FALSE(png4.empty());

    // 2. RGBA 3x3: packed row = 12 bytes. Stride = 13 (odd non-multiple of 4)
    std::vector<uint8_t> rgba_stride13(13 * 3, 200);
    auto png5 = ulv::compress_rgba_to_png(rgba_stride13.data(), 3, 3, 13);
    ASSERT_FALSE(png5.empty());
    ASSERT_EQ(read_be32(png5.data() + 16), 3);
    ASSERT_EQ(read_be32(png5.data() + 20), 3);

    // Stride = 15 (odd non-multiple of 4)
    std::vector<uint8_t> rgba_stride15(15 * 3, 220);
    auto png6 = ulv::compress_rgba_to_png(rgba_stride15.data(), 3, 3, 15);
    ASSERT_FALSE(png6.empty());
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Stress: 4K (4096x2160) Ultra-High Resolution RGBA") {
    const int w4k = 4096;
    const int h4k = 2160;
    // 4096 * 2160 * 4 = 35,389,440 bytes (~35.4 MB)
    std::vector<uint8_t> img4k(w4k * h4k * 4, 128);

    auto t0 = std::chrono::steady_clock::now();
    auto png4k = ulv::compress_rgba_to_png(img4k.data(), w4k, h4k);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    ASSERT_FALSE(png4k.empty());
    ASSERT_GT(png4k.size(), 1000);
    ASSERT_EQ(read_be32(png4k.data() + 16), static_cast<uint32_t>(w4k));
    ASSERT_EQ(read_be32(png4k.data() + 20), static_cast<uint32_t>(h4k));

    // Data URI generation on 4K image
    std::string data_uri = ulv::encode_png_to_data_uri(png4k.data(), png4k.size());
    ASSERT_TRUE(data_uri.rfind("data:image/png;base64,", 0) == 0);
    ASSERT_EQ(data_uri.size(), 22 + 4 * ((png4k.size() + 2) / 3));
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Fuzzing: Corrupted Base64 Characters & Control Codes") {
    // Invalid characters: punctuation
    const char* bad_chars = "!@#$%^&*()-_~`[]{}|\\;:'\"<>?";
    for (size_t i = 0; bad_chars[i] != '\0'; ++i) {
        std::string s = "Zm9v";
        s[2] = bad_chars[i];
        auto res = ulv::base64_decode(s);
        ASSERT_TRUE(res.empty());
    }

    // Control codes 0x00..0x1F (excluding \t=9, \n=10, \r=13)
    for (int c = 0; c < 32; ++c) {
        if (c == 9 || c == 10 || c == 13) continue;
        std::string s = "Zm9v";
        s[1] = static_cast<char>(c);
        auto res = ulv::base64_decode(s);
        ASSERT_TRUE(res.empty());
    }

    // High-ASCII / UTF-8 multi-byte sequences
    std::string utf8_chinese = "Zm9v点云";
    ASSERT_TRUE(ulv::base64_decode(utf8_chinese).empty());

    std::string utf8_emoji = "Zm9v🎉";
    ASSERT_TRUE(ulv::base64_decode(utf8_emoji).empty());

    for (int c = 128; c < 256; ++c) {
        std::string s = "Zm9v";
        s[3] = static_cast<char>(c);
        auto res = ulv::base64_decode(s);
        ASSERT_TRUE(res.empty());
    }
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Fuzzing: Malformed Padding & Length Anomalies") {
    // Strings of lengths not multiple of 4
    ASSERT_TRUE(ulv::base64_decode("").empty());
    ASSERT_TRUE(ulv::base64_decode("A").empty()); // 1 char cannot form a byte

    // 2 chars ("AA") = 12 bits -> 1 byte (0x00)
    auto res_aa = ulv::base64_decode("AA");
    ASSERT_EQ(res_aa.size(), 1);
    ASSERT_EQ(res_aa[0], 0);

    // Padding character alone
    ASSERT_TRUE(ulv::base64_decode("=").empty());
    ASSERT_TRUE(ulv::base64_decode("==").empty());
    ASSERT_TRUE(ulv::base64_decode("===").empty());
    ASSERT_TRUE(ulv::base64_decode("====").empty());
    ASSERT_TRUE(ulv::base64_decode("=====").empty());
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Fuzzing: RFC 4648 Whitespace Permutations") {
    // Valid Base64 "Zm9v" (foo) with scattered whitespace
    std::string s1 = " \t Zm9v \r\n ";
    auto r1 = ulv::base64_decode(s1);
    ASSERT_EQ(r1.size(), 3);
    std::string str1(r1.begin(), r1.end());
    ASSERT_EQ(str1, "foo");

    // Interleaved whitespace
    std::string s2 = "Z m\n9\r v";
    auto r2 = ulv::base64_decode(s2);
    ASSERT_EQ(r2.size(), 3);
    std::string str2(r2.begin(), r2.end());
    ASSERT_EQ(str2, "foo");

    // Whitespace only
    ASSERT_TRUE(ulv::base64_decode("   \t\r\n   ").empty());
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Fuzzing: Data URI Prefix Handling & Case Sensitivity") {
    // Valid Data URI
    std::string valid_uri = "data:image/png;base64,Zm9v";
    auto r1 = ulv::base64_decode(valid_uri);
    ASSERT_EQ(r1.size(), 3);
    std::string s1(r1.begin(), r1.end());
    ASSERT_EQ(s1, "foo");

    // Invalid MIME type: data:image/jpeg;base64,
    // Does not match PNG prefix, falls through to base64 decoder where ':' and '/' are rejected
    std::string jpeg_uri = "data:image/jpeg;base64,Zm9v";
    auto r2 = ulv::base64_decode(jpeg_uri);
    ASSERT_TRUE(r2.empty());

    // Truncated prefix: data:image/png;base64
    std::string trunc_prefix = "data:image/png;base64";
    auto r3 = ulv::base64_decode(trunc_prefix);
    ASSERT_TRUE(r3.empty());
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Stress: 10,000 Random Byte Patterns Roundtrip Fuzzing") {
    std::mt19937 rng(0x5EED1337);

    for (int iter = 0; iter < 10000; ++iter) {
        // Test edge-case lengths first, then random lengths up to 2048
        size_t len;
        if (iter < 20) {
            len = iter;
        } else if (iter < 40) {
            len = 250 + (iter - 20);
        } else {
            len = rng() % 1024;
        }

        std::vector<uint8_t> data(len);
        for (size_t i = 0; i < len; ++i) {
            data[i] = static_cast<uint8_t>(rng() % 256);
        }

        std::string encoded = ulv::base64_encode(data.data(), data.size());
        if (len == 0) {
            ASSERT_TRUE(encoded.empty());
        } else {
            ASSERT_EQ(encoded.size() % 4, 0);
            ASSERT_EQ(encoded.size(), 4 * ((len + 2) / 3));
        }

        std::vector<uint8_t> decoded = ulv::base64_decode(encoded);
        ASSERT_EQ(decoded.size(), data.size());
        ASSERT_TRUE(decoded == data);
    }
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder PNG Validation: RFC 2083 Structure & CRC-32") {
    const int w = 64, h = 32;
    std::vector<uint8_t> rgba(w * h * 4, 180);
    auto png = ulv::compress_rgba_to_png(rgba.data(), w, h);

    // 1. Signature
    const uint8_t kPngMagic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    ASSERT_GT(png.size(), 45);
    ASSERT_EQ(std::memcmp(png.data(), kPngMagic, 8), 0);

    // 2. IHDR Chunk
    size_t offset = 8;
    uint32_t ihdr_len = read_be32(png.data() + offset);
    ASSERT_EQ(ihdr_len, 13);
    ASSERT_EQ(std::memcmp(png.data() + offset + 4, "IHDR", 4), 0);
    uint32_t ihdr_w = read_be32(png.data() + offset + 8);
    uint32_t ihdr_h = read_be32(png.data() + offset + 12);
    ASSERT_EQ(ihdr_w, static_cast<uint32_t>(w));
    ASSERT_EQ(ihdr_h, static_cast<uint32_t>(h));
    ASSERT_EQ(png[offset + 16], 8); // bit depth
    ASSERT_EQ(png[offset + 17], 6); // RGBA
    ASSERT_EQ(png[offset + 18], 0); // compression
    ASSERT_EQ(png[offset + 19], 0); // filter
    ASSERT_EQ(png[offset + 20], 0); // interlace

    // Check IHDR CRC-32
    uint32_t expected_ihdr_crc = compute_crc32(png.data() + offset + 4, 4 + 13);
    uint32_t actual_ihdr_crc = read_be32(png.data() + offset + 21);
    ASSERT_EQ(expected_ihdr_crc, actual_ihdr_crc);
    offset += 4 + 4 + ihdr_len + 4; // 25 bytes

    // 3. IDAT Chunk
    uint32_t idat_len = read_be32(png.data() + offset);
    ASSERT_GT(idat_len, 0);
    ASSERT_EQ(std::memcmp(png.data() + offset + 4, "IDAT", 4), 0);
    uint32_t expected_idat_crc = compute_crc32(png.data() + offset + 4, 4 + idat_len);
    uint32_t actual_idat_crc = read_be32(png.data() + offset + 8 + idat_len);
    ASSERT_EQ(expected_idat_crc, actual_idat_crc);
    offset += 4 + 4 + idat_len + 4;

    // 4. IEND Chunk
    ASSERT_EQ(offset + 12, png.size());
    uint32_t iend_len = read_be32(png.data() + offset);
    ASSERT_EQ(iend_len, 0);
    ASSERT_EQ(std::memcmp(png.data() + offset + 4, "IEND", 4), 0);
    uint32_t actual_iend_crc = read_be32(png.data() + offset + 8);
    ASSERT_EQ(actual_iend_crc, 0xAE426082); // Constant CRC for 0-byte IEND chunk
}

TEST_CASE_TIER("Milestone 4 Challenge", "ImageEncoder Memory Safety: 1,000 Rapid Compression & Codec Cycles") {
    std::vector<uint8_t> rgba(128 * 72 * 4, 100);
    std::vector<uint8_t> rgb(64 * 64 * 3, 150);

    for (int i = 0; i < 1000; ++i) {
        auto p1 = ulv::compress_rgba_to_png(rgba.data(), 128, 72);
        ASSERT_FALSE(p1.empty());

        auto p2 = ulv::compress_rgb_to_png(rgb.data(), 64, 64);
        ASSERT_FALSE(p2.empty());

        std::string uri1 = ulv::encode_png_to_data_uri(p1.data(), p1.size());
        ASSERT_FALSE(uri1.empty());

        std::string uri2 = ulv::rgba_to_png_data_uri(rgba.data(), 128, 72, 0, false);
        ASSERT_FALSE(uri2.empty());

        auto dec = ulv::base64_decode(uri1);
        ASSERT_EQ(dec.size(), p1.size());
    }
}
