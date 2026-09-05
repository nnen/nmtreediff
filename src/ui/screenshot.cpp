/// \file
/// \brief Implementation of bitmap writing.

#include "ui/screenshot.h"

#include <fstream>

namespace nmxd {

namespace {

/// \brief Appends a little-endian 16-bit value to a byte buffer.
///
/// \param out The buffer to append to.
/// \param value The value to append.
void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

/// \brief Appends a little-endian 32-bit value to a byte buffer.
///
/// \param out The buffer to append to.
/// \param value The value to append.
void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

}  // namespace

bool writeBitmap(const std::filesystem::path& path, int width, int height,
                 const std::vector<std::uint8_t>& rgba) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (rgba.size() < pixels * 4) {
        return false;
    }

    // Each row is padded to a four-byte boundary, which the format requires.
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 3;
    const std::size_t padding = (4 - (rowBytes % 4)) % 4;
    const std::size_t imageBytes = (rowBytes + padding) * static_cast<std::size_t>(height);

    std::vector<std::uint8_t> file;
    file.reserve(54 + imageBytes);

    file.push_back('B');
    file.push_back('M');
    put32(file, static_cast<std::uint32_t>(54 + imageBytes));
    put16(file, 0);
    put16(file, 0);
    put32(file, 54);

    put32(file, 40);
    put32(file, static_cast<std::uint32_t>(width));
    put32(file, static_cast<std::uint32_t>(height));
    put16(file, 1);
    put16(file, 24);
    put32(file, 0);
    put32(file, static_cast<std::uint32_t>(imageBytes));
    put32(file, 2835);
    put32(file, 2835);
    put32(file, 0);
    put32(file, 0);

    for (int y = 0; y < height; ++y) {
        const std::size_t rowStart = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            const std::size_t at = rowStart + static_cast<std::size_t>(x) * 4;
            // Bitmaps store blue, green then red.
            file.push_back(rgba[at + 2]);
            file.push_back(rgba[at + 1]);
            file.push_back(rgba[at + 0]);
        }
        for (std::size_t p = 0; p < padding; ++p) {
            file.push_back(0);
        }
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return out.good();
}

}  // namespace nmxd
