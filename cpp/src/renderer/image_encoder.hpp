#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ulv {

/**
 * @brief Thread-safe vertical row flip for raw image buffers (OpenGL bottom-left to top-left).
 * @param pixels Pointer to pixel data (modified in-place)
 * @param width Image width in pixels
 * @param height Image height in pixels
 * @param channels Number of bytes per pixel (e.g. 4 for RGBA, 3 for RGB)
 */
void flip_image_vertical(uint8_t* pixels, int width, int height, int channels);

/**
 * @brief Compress raw RGBA buffer into PNG memory format (zero disk I/O).
 * @param rgba_pixels Pointer to RGBA pixel array (width * height * 4 bytes)
 * @param width Image width in pixels
 * @param height Image height in pixels
 * @param stride_bytes Row stride in bytes (0 defaults to width * 4)
 * @return Vector of compressed PNG file bytes, or empty vector on failure
 */
std::vector<uint8_t> compress_rgba_to_png(const uint8_t* rgba_pixels, int width, int height, int stride_bytes = 0);

/**
 * @brief Compress raw RGB buffer into PNG memory format (zero disk I/O).
 * @param rgb_pixels Pointer to RGB pixel array (width * height * 3 bytes)
 * @param width Image width in pixels
 * @param height Image height in pixels
 * @param stride_bytes Row stride in bytes (0 defaults to width * 3)
 * @return Vector of compressed PNG file bytes, or empty vector on failure
 */
std::vector<uint8_t> compress_rgb_to_png(const uint8_t* rgb_pixels, int width, int height, int stride_bytes = 0);

/**
 * @brief Encode binary data into standard RFC 4648 Base64 string with '=' padding.
 * @param data Pointer to binary data buffer
 * @param len Size of data buffer in bytes
 * @return Base64 encoded string
 */
std::string base64_encode(const uint8_t* data, size_t len);

/**
 * @brief Decode RFC 4648 Base64 string into raw binary bytes.
 * Automatically ignores leading "data:image/png;base64," prefix and ASCII whitespace.
 * @param base64_str Input Base64 string or Data URI
 * @return Decoded byte vector, or empty vector on invalid syntax
 */
std::vector<uint8_t> base64_decode(std::string_view base64_str);

/**
 * @brief Direct zero-copy encoder from binary PNG bytes into RFC 2397 Data URI string:
 *        "data:image/png;base64," + base64_str.
 * @param png_data Pointer to binary PNG bytes
 * @param len Byte count of PNG data
 * @return Complete Data URI string
 */
std::string encode_png_to_data_uri(const uint8_t* png_data, size_t len);

/**
 * @brief End-to-end pipeline: Compresses RGBA buffer to PNG and directly produces Base64 Data URI.
 * Performs zero intermediate disk I/O and zero redundant buffer copies.
 * @param rgba_pixels Pointer to RGBA pixel array
 * @param width Image width in pixels
 * @param height Image height in pixels
 * @param stride_bytes Row stride in bytes (0 defaults to width * 4)
 * @param flip_vertical If true, flips rows vertically prior to PNG encoding
 * @return Complete "data:image/png;base64,..." URI string ready for HTML embedding
 */
std::string rgba_to_png_data_uri(const uint8_t* rgba_pixels, int width, int height, int stride_bytes = 0, bool flip_vertical = false);

} // namespace ulv
