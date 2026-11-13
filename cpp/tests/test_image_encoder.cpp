#include "test_framework.hpp"
#include "renderer/image_encoder.hpp"

#include <cstring>
#include <vector>

TEST_CASE_TIER("Tier 1: Feature Coverage", "ImageEncoder: RFC 4648 Base64 Test Vectors") {
    struct TestVector {
        const char* plain;
        const char* expected;
    };

    TestVector vectors[] = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"}
    };

    for (const auto& vec : vectors) {
        std::string encoded = ulv::base64_encode(
            reinterpret_cast<const uint8_t*>(vec.plain),
            std::strlen(vec.plain)
        );
        ASSERT_EQ(encoded, vec.expected);

        std::vector<uint8_t> decoded = ulv::base64_decode(encoded);
        std::string roundtrip(decoded.begin(), decoded.end());
        ASSERT_EQ(roundtrip, vec.plain);
    }
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "ImageEncoder: Base64 Binary 256-Byte Roundtrip") {
    std::vector<uint8_t> raw_binary(256);
    for (size_t i = 0; i < 256; ++i) {
        raw_binary[i] = static_cast<uint8_t>(i);
    }

    std::string b64 = ulv::base64_encode(raw_binary.data(), raw_binary.size());
    ASSERT_FALSE(b64.empty());
    ASSERT_EQ(b64.size() % 4, 0);

    std::vector<uint8_t> decoded = ulv::base64_decode(b64);
    ASSERT_EQ(decoded.size(), raw_binary.size());
    ASSERT_TRUE(decoded == raw_binary);
}

TEST_CASE_TIER("Tier 1: Feature Coverage", "ImageEncoder: Synthetic RGBA PNG In-Memory Compression") {
    // Construct a synthetic 8x8 RGBA test gradient
    const int w = 8, h = 8;
    std::vector<uint8_t> rgba(w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = (y * w + x) * 4;
            rgba[idx + 0] = static_cast<uint8_t>(x * 32); // R
            rgba[idx + 1] = static_cast<uint8_t>(y * 32); // G
            rgba[idx + 2] = 200;                           // B
            rgba[idx + 3] = 255;                           // A
        }
    }

    std::vector<uint8_t> png_bytes = ulv::compress_rgba_to_png(rgba.data(), w, h);
    ASSERT_GT(png_bytes.size(), 16);

    // Validate standard PNG 8-byte file magic signature: \x89PNG\r\n\x1a\n
    const uint8_t expected_magic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    ASSERT_EQ(std::memcmp(png_bytes.data(), expected_magic, 8), 0);
}

TEST_CASE_TIER("Tier 3: Cross-Feature Integration", "ImageEncoder: Data URI Generation and Decoded PNG Header") {
    const int w = 16, h = 16;
    std::vector<uint8_t> rgba(w * h * 4, 128); // 16x16 solid grey

    std::string uri = ulv::rgba_to_png_data_uri(rgba.data(), w, h, 0, false);
    ASSERT_TRUE(uri.rfind("data:image/png;base64,", 0) == 0);

    std::vector<uint8_t> decoded_png = ulv::base64_decode(uri);
    ASSERT_GT(decoded_png.size(), 8);

    const uint8_t expected_magic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    ASSERT_EQ(std::memcmp(decoded_png.data(), expected_magic, 8), 0);
}

TEST_CASE_TIER("Tier 2: Boundary & Corner", "ImageEncoder: In-Place Vertical Flip Accuracy") {
    // 2x2 image with distinct 4-byte pixels:
    // Top row:    [1, 1, 1, 1] [2, 2, 2, 2]
    // Bottom row: [3, 3, 3, 3] [4, 4, 4, 4]
    std::vector<uint8_t> img = {
        1, 1, 1, 1,   2, 2, 2, 2,
        3, 3, 3, 3,   4, 4, 4, 4
    };

    ulv::flip_image_vertical(img.data(), 2, 2, 4);

    // After vertical flip:
    // Top row should be [3, 3, 3, 3] [4, 4, 4, 4]
    // Bottom row should be [1, 1, 1, 1] [2, 2, 2, 2]
    std::vector<uint8_t> expected = {
        3, 3, 3, 3,   4, 4, 4, 4,
        1, 1, 1, 1,   2, 2, 2, 2
    };

    ASSERT_TRUE(img == expected);
}
