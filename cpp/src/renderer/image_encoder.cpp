#include "image_encoder.hpp"

#include <cstring>
#include <memory>
#include <utility>

// Suppress legacy sprintf warnings from stb_image_write in third-party inclusion
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ulv {

namespace {

// RAII holder for heap buffer allocated by stbi_write_png_to_mem (STBIW_MALLOC)
struct StbiMemGuard {
    unsigned char* ptr = nullptr;
    explicit StbiMemGuard(unsigned char* p) : ptr(p) {}
    ~StbiMemGuard() {
        if (ptr) {
            STBIW_FREE(ptr);
        }
    }
    StbiMemGuard(const StbiMemGuard&) = delete;
    StbiMemGuard& operator=(const StbiMemGuard&) = delete;
    StbiMemGuard(StbiMemGuard&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
    StbiMemGuard& operator=(StbiMemGuard&& o) noexcept {
        if (this != &o) {
            if (ptr) STBIW_FREE(ptr);
            ptr = o.ptr;
            o.ptr = nullptr;
        }
        return *this;
    }
};

static constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static constexpr std::string_view kPngDataUriPrefix = "data:image/png;base64,";

// RFC 4648 decode table mapping ASCII uint8 to 6-bit value (0..63)
// -1: padding '=', -2: whitespace, -3: invalid character
static const int8_t kBase64DecodeTable[256] = {
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-2,-2,-3,-3,-2,-3,-3, // 0..15 (\t=9, \n=10, \r=13)
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3, // 16..31
    -2,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,62,-3,-3,-3,63, // 32..47 (' '=32, '+'=43, '/'=47)
    52,53,54,55,56,57,58,59,60,61,-3,-3,-3,-1,-3,-3, // 48..63 ('0'..'9'=52..61, '='=61 is -1)
    -3, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, // 64..79 ('@', 'A'..'O')
    15,16,17,18,19,20,21,22,23,24,25,-3,-3,-3,-3,-3, // 80..95 ('P'..'Z')
    -3,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, // 96..111 ('`', 'a'..'o')
    41,42,43,44,45,46,47,48,49,50,51,-3,-3,-3,-3,-3, // 112..127 ('p'..'z')
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3, // 128..143
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3, // 144..159
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3, // 160..175
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3, // 176..191
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3, // 192..207
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3, // 208..223
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3, // 224..239
    -3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3  // 240..255
};

} // namespace

void flip_image_vertical(uint8_t* pixels, int width, int height, int channels) {
    if (!pixels || width <= 0 || height <= 0 || channels <= 0) return;
    const size_t row_bytes = static_cast<size_t>(width) * channels;
    std::vector<uint8_t> temp_row(row_bytes);

    for (int y = 0; y < height / 2; ++y) {
        uint8_t* top = pixels + y * row_bytes;
        uint8_t* bottom = pixels + (height - 1 - y) * row_bytes;
        std::memcpy(temp_row.data(), top, row_bytes);
        std::memcpy(top, bottom, row_bytes);
        std::memcpy(bottom, temp_row.data(), row_bytes);
    }
}

std::vector<uint8_t> compress_rgba_to_png(const uint8_t* rgba_pixels, int width, int height, int stride_bytes) {
    if (!rgba_pixels || width <= 0 || height <= 0) return {};
    if (stride_bytes < 0 || (stride_bytes != 0 && stride_bytes < width * 4)) return {};
    int out_len = 0;
    unsigned char* raw_png = stbi_write_png_to_mem(rgba_pixels, stride_bytes, width, height, 4, &out_len);
    StbiMemGuard guard(raw_png);
    if (!raw_png || out_len <= 0) return {};
    return std::vector<uint8_t>(raw_png, raw_png + out_len);
}

std::vector<uint8_t> compress_rgb_to_png(const uint8_t* rgb_pixels, int width, int height, int stride_bytes) {
    if (!rgb_pixels || width <= 0 || height <= 0) return {};
    if (stride_bytes < 0 || (stride_bytes != 0 && stride_bytes < width * 3)) return {};
    int out_len = 0;
    unsigned char* raw_png = stbi_write_png_to_mem(rgb_pixels, stride_bytes, width, height, 3, &out_len);
    StbiMemGuard guard(raw_png);
    if (!raw_png || out_len <= 0) return {};
    return std::vector<uint8_t>(raw_png, raw_png + out_len);
}

std::string base64_encode(const uint8_t* data, size_t len) {
    if (!data || len == 0) return {};

    const size_t out_len = 4 * ((len + 2) / 3);
    std::string result;
    result.resize(out_len);
    char* dst = result.data();

    size_t i = 0;
    while (i + 2 < len) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                                (static_cast<uint32_t>(data[i + 1]) << 8) |
                                (static_cast<uint32_t>(data[i + 2]));
        *dst++ = kBase64Alphabet[(triple >> 18) & 0x3F];
        *dst++ = kBase64Alphabet[(triple >> 12) & 0x3F];
        *dst++ = kBase64Alphabet[(triple >> 6) & 0x3F];
        *dst++ = kBase64Alphabet[triple & 0x3F];
        i += 3;
    }

    if (i < len) {
        const size_t rem = len - i;
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        if (rem == 2) {
            triple |= static_cast<uint32_t>(data[i + 1]) << 8;
        }
        *dst++ = kBase64Alphabet[(triple >> 18) & 0x3F];
        *dst++ = kBase64Alphabet[(triple >> 12) & 0x3F];
        if (rem == 2) {
            *dst++ = kBase64Alphabet[(triple >> 6) & 0x3F];
            *dst++ = '=';
        } else {
            *dst++ = '=';
            *dst++ = '=';
        }
    }

    return result;
}

std::vector<uint8_t> base64_decode(std::string_view base64_str) {
    if (base64_str.rfind(kPngDataUriPrefix, 0) == 0) {
        base64_str.remove_prefix(kPngDataUriPrefix.size());
    }

    std::vector<uint8_t> out;
    out.reserve((base64_str.size() * 3) / 4);

    uint32_t val = 0;
    int valb = -8;
    for (char c : base64_str) {
        const uint8_t uc = static_cast<uint8_t>(c);
        const int8_t d = kBase64DecodeTable[uc];
        if (d == -2) continue; // skip whitespace
        if (d == -1) break;    // '=' padding encountered
        if (d < 0) return {};  // syntax error

        val = (val << 6) | static_cast<uint32_t>(d);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string encode_png_to_data_uri(const uint8_t* png_data, size_t len) {
    if (!png_data || len == 0) {
        return std::string(kPngDataUriPrefix);
    }

    const size_t b64_len = 4 * ((len + 2) / 3);
    std::string result;
    result.resize(kPngDataUriPrefix.size() + b64_len);

    std::memcpy(result.data(), kPngDataUriPrefix.data(), kPngDataUriPrefix.size());
    char* dst = result.data() + kPngDataUriPrefix.size();

    size_t i = 0;
    while (i + 2 < len) {
        const uint32_t triple = (static_cast<uint32_t>(png_data[i]) << 16) |
                                (static_cast<uint32_t>(png_data[i + 1]) << 8) |
                                (static_cast<uint32_t>(png_data[i + 2]));
        *dst++ = kBase64Alphabet[(triple >> 18) & 0x3F];
        *dst++ = kBase64Alphabet[(triple >> 12) & 0x3F];
        *dst++ = kBase64Alphabet[(triple >> 6) & 0x3F];
        *dst++ = kBase64Alphabet[triple & 0x3F];
        i += 3;
    }

    if (i < len) {
        const size_t rem = len - i;
        uint32_t triple = static_cast<uint32_t>(png_data[i]) << 16;
        if (rem == 2) {
            triple |= static_cast<uint32_t>(png_data[i + 1]) << 8;
        }
        *dst++ = kBase64Alphabet[(triple >> 18) & 0x3F];
        *dst++ = kBase64Alphabet[(triple >> 12) & 0x3F];
        if (rem == 2) {
            *dst++ = kBase64Alphabet[(triple >> 6) & 0x3F];
            *dst++ = '=';
        } else {
            *dst++ = '=';
            *dst++ = '=';
        }
    }

    return result;
}

std::string rgba_to_png_data_uri(const uint8_t* rgba_pixels, int width, int height, int stride_bytes, bool flip_vertical) {
    if (!rgba_pixels || width <= 0 || height <= 0) return {};
    if (stride_bytes < 0 || (stride_bytes != 0 && stride_bytes < width * 4)) return {};

    const uint8_t* source_pixels = rgba_pixels;
    std::vector<uint8_t> flipped_buf;

    if (flip_vertical) {
        const size_t packed_row = static_cast<size_t>(width) * 4;
        const size_t src_stride = (stride_bytes > 0) ? static_cast<size_t>(stride_bytes) : packed_row;
        flipped_buf.resize(packed_row * height);
        for (int y = 0; y < height; ++y) {
            std::memcpy(flipped_buf.data() + y * packed_row, rgba_pixels + y * src_stride, packed_row);
        }
        flip_image_vertical(flipped_buf.data(), width, height, 4);
        source_pixels = flipped_buf.data();
        stride_bytes = 0;
    }

    int out_len = 0;
    unsigned char* raw_png = stbi_write_png_to_mem(source_pixels, stride_bytes, width, height, 4, &out_len);
    StbiMemGuard guard(raw_png);

    if (!raw_png || out_len <= 0) return {};
    return encode_png_to_data_uri(raw_png, static_cast<size_t>(out_len));
}

} // namespace ulv
