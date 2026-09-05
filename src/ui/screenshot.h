#pragma once

/// \file
/// \brief Saving the rendered window to an image file.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace nmxd {

/// \brief Writes pixels to an uncompressed 24-bit bitmap file.
///
/// \param path Where to write the file.
/// \param width Image width in pixels.
/// \param height Image height in pixels.
/// \param rgba Pixel data, four bytes per pixel, bottom row first as OpenGL
///        reads it.
///
/// \returns `true` when the file was written.
///
/// \remarks Bitmap rather than a compressed format so that saving a frame costs
///          no dependency. Rows are written bottom-up, which is both what the
///          format wants and what the framebuffer already gives.
[[nodiscard]] bool writeBitmap(const std::filesystem::path& path, int width, int height,
                               const std::vector<std::uint8_t>& rgba);

}  // namespace nmxd
