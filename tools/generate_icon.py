#!/usr/bin/env python3
"""
@file generate_icon.py
@brief Generates the NM Tree Diff application icon as Netpbm images and as a
       Windows icon.

The icon shows a small left-to-right tree in the style of the node view: an
unchanged root node whose three children are a changed (amber), an added
(green) and a removed (red) node.

The icon is described as a list of shapes in a fixed 256 x 256 design space
and rasterized with signed distance functions, so it can be rendered
anti-aliased at any size using nothing but the Python standard library.

Usage:
    python tools/generate_icon.py [--output-dir DIR] [--sizes N [N ...]]
                                  [--ico-sizes N [N ...]]

For every size in --sizes two files are written:
    nmtreediff_<N>.pam  Binary PAM (P7, RGB_ALPHA) with transparent corners.
    nmtreediff_<N>.ppm  Binary PPM (P6), opaque, corners filled with the
                        background color.

In addition a single Windows icon is written:
    nmtreediff.ico      All sizes in --ico-sizes, each rendered natively
                        rather than scaled down, with transparent corners.
"""

import argparse
import math
import os
import struct
import zlib

# ---------------------------------------------------------------------------
# Design space
# ---------------------------------------------------------------------------

## Edge length of the square design space all shape coordinates refer to.
DESIGN_SIZE = 256.0

## Maximum value of a color channel in the generated images.
CHANNEL_MAX = 255

## Default edge lengths, in pixels, of the generated icons.
DEFAULT_SIZES = [256]

## Default edge lengths, in pixels, of the images inside the Windows icon.
DEFAULT_ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]

## Default directory the icons are written to, relative to the repository root.
DEFAULT_OUTPUT_DIR = os.path.join("assets", "icon")

## Base name of the generated files.
ICON_BASE_NAME = "nmtreediff"

# ---------------------------------------------------------------------------
# Palette (matches the colors of the application's node view)
# ---------------------------------------------------------------------------

COLOR_BACKGROUND = (22, 24, 32)
COLOR_BACKGROUND_BORDER = (58, 62, 80)
COLOR_EDGE = (134, 140, 160)
COLOR_ROOT_FILL = (46, 50, 66)
COLOR_ROOT_BORDER = (168, 174, 196)
COLOR_CHANGED_FILL = (88, 70, 38)
COLOR_CHANGED_BORDER = (236, 184, 80)
COLOR_ADDED_FILL = (34, 76, 60)
COLOR_ADDED_BORDER = (84, 204, 142)
COLOR_REMOVED_FILL = (88, 40, 48)
COLOR_REMOVED_BORDER = (236, 104, 116)

# ---------------------------------------------------------------------------
# Layout (all values in design space units)
# ---------------------------------------------------------------------------

## Corner radius of the icon's background plate.
BACKGROUND_RADIUS = 52.0

## Border thickness of the icon's background plate.
BACKGROUND_BORDER = 4.0

## Horizontal extent of the root node.
ROOT_LEFT, ROOT_RIGHT = 26.0, 98.0

## Horizontal extent of the child nodes.
CHILD_LEFT, CHILD_RIGHT = 150.0, 230.0

## Height of every node.
NODE_HEIGHT = 50.0

## Corner radius of every node.
NODE_RADIUS = 9.0

## Border thickness of every node.
NODE_BORDER = 6.0

## Vertical centers of the three child nodes, top to bottom.
CHILD_CENTERS_Y = [54.0, 128.0, 202.0]

## Vertical center of the root node.
ROOT_CENTER_Y = 128.0

## Thickness of the edges connecting the nodes.
EDGE_THICKNESS = 6.0

## Number of line segments a curved edge is flattened into.
EDGE_SEGMENTS = 24

## Half of the length of the strokes making up the "+" and "-" glyphs.
GLYPH_HALF_LENGTH = 13.0

## Thickness of the glyph strokes.
GLYPH_THICKNESS = 7.0

## Half of the width of the "~" glyph.
TILDE_HALF_WIDTH = 17.0

## Amplitude of the "~" glyph's wave.
TILDE_AMPLITUDE = 6.0

## Number of line segments the "~" glyph is flattened into.
TILDE_SEGMENTS = 16


# ---------------------------------------------------------------------------
# Shapes
# ---------------------------------------------------------------------------

class RoundedRect:
    """
    @brief A filled rectangle with rounded corners.
    """

    def __init__(self, left, top, right, bottom, radius, color):
        """
        @brief Initializes the rectangle.

        @param left   Left edge in design space.
        @param top    Top edge in design space.
        @param right  Right edge in design space.
        @param bottom Bottom edge in design space.
        @param radius Corner radius in design space.
        @param color  Fill color as an (r, g, b) tuple.
        """
        self.center_x = (left + right) / 2.0
        self.center_y = (top + bottom) / 2.0
        self.half_width = (right - left) / 2.0 - radius
        self.half_height = (bottom - top) / 2.0 - radius
        self.radius = radius
        self.color = color

    def distance(self, x, y):
        """
        @brief Computes the signed distance from a point to the shape.

        @param x Horizontal coordinate of the point in design space.
        @param y Vertical coordinate of the point in design space.

        @return Distance to the outline; negative inside the shape.
        """
        qx = abs(x - self.center_x) - self.half_width
        qy = abs(y - self.center_y) - self.half_height
        outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
        inside = min(max(qx, qy), 0.0)
        return outside + inside - self.radius


class Stroke:
    """
    @brief A polyline drawn with a round brush.
    """

    def __init__(self, points, thickness, color):
        """
        @brief Initializes the stroke.

        @param points    List of (x, y) vertices in design space.
        @param thickness Brush diameter in design space.
        @param color     Stroke color as an (r, g, b) tuple.
        """
        self.segments = list(zip(points[:-1], points[1:]))
        self.half_thickness = thickness / 2.0
        self.color = color

    def distance(self, x, y):
        """
        @brief Computes the signed distance from a point to the shape.

        @param x Horizontal coordinate of the point in design space.
        @param y Vertical coordinate of the point in design space.

        @return Distance to the outline; negative inside the shape.
        """
        nearest = min(segment_distance(x, y, a, b) for a, b in self.segments)
        return nearest - self.half_thickness


def segment_distance(x, y, start, end):
    """
    @brief Computes the distance from a point to a line segment.

    @param x     Horizontal coordinate of the point.
    @param y     Vertical coordinate of the point.
    @param start (x, y) start of the segment.
    @param end   (x, y) end of the segment.

    @return Unsigned distance from the point to the nearest point of the
            segment.
    """
    dx, dy = end[0] - start[0], end[1] - start[1]
    px, py = x - start[0], y - start[1]
    length_squared = dx * dx + dy * dy

    # Project the point onto the segment and clamp to its end points.
    t = 0.0
    if length_squared > 0.0:
        t = max(0.0, min(1.0, (px * dx + py * dy) / length_squared))
    return math.hypot(px - t * dx, py - t * dy)


# ---------------------------------------------------------------------------
# Icon description
# ---------------------------------------------------------------------------

def bordered_rect(left, top, right, bottom, radius, border, fill_color,
                  border_color):
    """
    @brief Builds a rounded rectangle with a border.

    @param left         Left edge in design space.
    @param top          Top edge in design space.
    @param right        Right edge in design space.
    @param bottom       Bottom edge in design space.
    @param radius       Outer corner radius.
    @param border       Border thickness.
    @param fill_color   Color of the interior.
    @param border_color Color of the border.

    @return List of shapes, in painting order.
    """
    inner_radius = max(radius - border, 0.0)
    return [
        RoundedRect(left, top, right, bottom, radius, border_color),
        RoundedRect(left + border, top + border, right - border,
                    bottom - border, inner_radius, fill_color),
    ]


def node(left, right, center_y, fill_color, border_color):
    """
    @brief Builds a tree node.

    @param left         Left edge in design space.
    @param right        Right edge in design space.
    @param center_y     Vertical center in design space.
    @param fill_color   Color of the node's interior.
    @param border_color Color of the node's border.

    @return List of shapes, in painting order.
    """
    half_height = NODE_HEIGHT / 2.0
    return bordered_rect(left, center_y - half_height, right,
                         center_y + half_height, NODE_RADIUS, NODE_BORDER,
                         fill_color, border_color)


def edge(child_center_y):
    """
    @brief Builds the curved edge from the root node to a child node.

    @param child_center_y Vertical center of the child node.

    @return The edge as a Stroke.
    """
    # Cubic Bezier leaving the root and entering the child horizontally.
    middle_x = (ROOT_RIGHT + CHILD_LEFT) / 2.0
    controls = [(ROOT_RIGHT, ROOT_CENTER_Y), (middle_x, ROOT_CENTER_Y),
                (middle_x, child_center_y), (CHILD_LEFT, child_center_y)]

    points = [bezier_point(controls, i / EDGE_SEGMENTS)
              for i in range(EDGE_SEGMENTS + 1)]
    return Stroke(points, EDGE_THICKNESS, COLOR_EDGE)


def bezier_point(controls, t):
    """
    @brief Evaluates a cubic Bezier curve.

    @param controls The four (x, y) control points.
    @param t        Curve parameter in the range [0, 1].

    @return The (x, y) point on the curve.
    """
    u = 1.0 - t
    weights = (u * u * u, 3.0 * u * u * t, 3.0 * u * t * t, t * t * t)
    return (sum(w * c[0] for w, c in zip(weights, controls)),
            sum(w * c[1] for w, c in zip(weights, controls)))


def horizontal_bar(center_x, center_y, color):
    """
    @brief Builds the horizontal stroke shared by the "+" and "-" glyphs.

    @param center_x Horizontal center of the glyph.
    @param center_y Vertical center of the glyph.
    @param color    Glyph color.

    @return The bar as a Stroke.
    """
    return Stroke([(center_x - GLYPH_HALF_LENGTH, center_y),
                   (center_x + GLYPH_HALF_LENGTH, center_y)],
                  GLYPH_THICKNESS, color)


def vertical_bar(center_x, center_y, color):
    """
    @brief Builds the vertical stroke of the "+" glyph.

    @param center_x Horizontal center of the glyph.
    @param center_y Vertical center of the glyph.
    @param color    Glyph color.

    @return The bar as a Stroke.
    """
    return Stroke([(center_x, center_y - GLYPH_HALF_LENGTH),
                   (center_x, center_y + GLYPH_HALF_LENGTH)],
                  GLYPH_THICKNESS, color)


def tilde(center_x, center_y, color):
    """
    @brief Builds the "~" glyph marking a changed node.

    @param center_x Horizontal center of the glyph.
    @param center_y Vertical center of the glyph.
    @param color    Glyph color.

    @return The glyph as a Stroke.
    """
    points = []
    for i in range(TILDE_SEGMENTS + 1):
        # One full sine period across the glyph's width.
        phase = i / TILDE_SEGMENTS
        points.append((center_x + (2.0 * phase - 1.0) * TILDE_HALF_WIDTH,
                       center_y - TILDE_AMPLITUDE
                       * math.sin(2.0 * math.pi * phase)))
    return Stroke(points, GLYPH_THICKNESS, color)


def build_foreground():
    """
    @brief Builds everything painted on top of the background plate.

    @return List of shapes, in painting order.
    """
    changed_y, added_y, removed_y = CHILD_CENTERS_Y
    glyph_x = (CHILD_LEFT + CHILD_RIGHT) / 2.0

    # Edges go first so the nodes cover their ends.
    shapes = [edge(center_y) for center_y in CHILD_CENTERS_Y]

    # The unchanged root and its changed, added and removed children.
    shapes += node(ROOT_LEFT, ROOT_RIGHT, ROOT_CENTER_Y, COLOR_ROOT_FILL,
                   COLOR_ROOT_BORDER)
    shapes += node(CHILD_LEFT, CHILD_RIGHT, changed_y, COLOR_CHANGED_FILL,
                   COLOR_CHANGED_BORDER)
    shapes += node(CHILD_LEFT, CHILD_RIGHT, added_y, COLOR_ADDED_FILL,
                   COLOR_ADDED_BORDER)
    shapes += node(CHILD_LEFT, CHILD_RIGHT, removed_y, COLOR_REMOVED_FILL,
                   COLOR_REMOVED_BORDER)

    # Diff glyphs inside the children.
    shapes.append(tilde(glyph_x, changed_y, COLOR_CHANGED_BORDER))
    shapes.append(horizontal_bar(glyph_x, added_y, COLOR_ADDED_BORDER))
    shapes.append(vertical_bar(glyph_x, added_y, COLOR_ADDED_BORDER))
    shapes.append(horizontal_bar(glyph_x, removed_y, COLOR_REMOVED_BORDER))
    return shapes


def build_background():
    """
    @brief Builds the background plate.

    @return Tuple of (plate, shapes): the plate's outer shape, which defines
            the icon's alpha channel, and the list of shapes painting it.
    """
    shapes = bordered_rect(0.0, 0.0, DESIGN_SIZE, DESIGN_SIZE,
                           BACKGROUND_RADIUS, BACKGROUND_BORDER,
                           COLOR_BACKGROUND, COLOR_BACKGROUND_BORDER)
    return shapes[0], shapes


# ---------------------------------------------------------------------------
# Rasterization
# ---------------------------------------------------------------------------

def coverage(shape, x, y, pixels_per_unit):
    """
    @brief Computes how much of a pixel a shape covers.

    @param shape           The shape.
    @param x               Horizontal pixel center in design space.
    @param y               Vertical pixel center in design space.
    @param pixels_per_unit Size of a design space unit in pixels.

    @return Coverage in the range [0, 1].
    """
    # A pixel whose center lies exactly on the outline is half covered.
    return max(0.0, min(1.0, 0.5 - shape.distance(x, y) * pixels_per_unit))


def shade_pixel(plate, shapes, x, y, pixels_per_unit):
    """
    @brief Computes the color of a single pixel.

    @param plate           Shape defining the icon's alpha channel.
    @param shapes          All shapes, in painting order.
    @param x               Horizontal pixel center in design space.
    @param y               Vertical pixel center in design space.
    @param pixels_per_unit Size of a design space unit in pixels.

    @return The pixel as an (r, g, b, a) tuple of floats in [0, CHANNEL_MAX].
    """
    # Paint the shapes back to front over the background color.
    color = list(COLOR_BACKGROUND)
    for shape in shapes:
        amount = coverage(shape, x, y, pixels_per_unit)
        if amount > 0.0:
            color = [c + (s - c) * amount for c, s in zip(color, shape.color)]

    alpha = coverage(plate, x, y, pixels_per_unit) * CHANNEL_MAX
    return (color[0], color[1], color[2], alpha)


def render(size):
    """
    @brief Rasterizes the icon.

    @param size Edge length of the image in pixels.

    @return Row-major list of (r, g, b, a) integer tuples.
    """
    plate, shapes = build_background()
    shapes = shapes + build_foreground()
    pixels_per_unit = size / DESIGN_SIZE

    pixels = []
    for row in range(size):
        for column in range(size):
            # Sample at the pixel center.
            x = (column + 0.5) / pixels_per_unit
            y = (row + 0.5) / pixels_per_unit
            pixel = shade_pixel(plate, shapes, x, y, pixels_per_unit)
            pixels.append(tuple(int(round(channel)) for channel in pixel))
    return pixels


# ---------------------------------------------------------------------------
# Netpbm output
# ---------------------------------------------------------------------------

def write_pam(path, size, pixels):
    """
    @brief Writes an image as a binary PAM (P7) file with an alpha channel.

    @param path   Path of the file to write.
    @param size   Edge length of the image in pixels.
    @param pixels Row-major list of (r, g, b, a) tuples.
    """
    header = (f"P7\nWIDTH {size}\nHEIGHT {size}\nDEPTH 4\n"
              f"MAXVAL {CHANNEL_MAX}\nTUPLTYPE RGB_ALPHA\nENDHDR\n")
    with open(path, "wb") as file:
        file.write(header.encode("ascii"))
        file.write(bytes(channel for pixel in pixels for channel in pixel))


def write_ppm(path, size, pixels):
    """
    @brief Writes an image as a binary PPM (P6) file, discarding alpha.

    @param path   Path of the file to write.
    @param size   Edge length of the image in pixels.
    @param pixels Row-major list of (r, g, b, a) tuples.
    """
    header = f"P6\n{size} {size}\n{CHANNEL_MAX}\n"
    with open(path, "wb") as file:
        file.write(header.encode("ascii"))
        file.write(bytes(channel for pixel in pixels for channel in pixel[:3]))


# ---------------------------------------------------------------------------
# Windows icon output
# ---------------------------------------------------------------------------

## Signature every PNG stream starts with.
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

## Bits per channel of the PNG images.
PNG_BIT_DEPTH = 8

## PNG color type of truecolor images with alpha.
PNG_COLOR_TYPE_RGBA = 6

## PNG filter type byte of a scanline stored unfiltered.
PNG_FILTER_NONE = b"\x00"

## Size of the BITMAPINFOHEADER structure in bytes.
BMP_INFO_HEADER_SIZE = 40

## Bits per pixel of the bitmap images.
BMP_BITS_PER_PIXEL = 32

## Bit count the rows of a bitmap's 1-bit AND mask are padded to.
BMP_MASK_ROW_ALIGNMENT = 32

## Value of the ICONDIR type field identifying an icon (as opposed to a cursor).
ICO_TYPE_ICON = 1

## Size of the ICONDIR structure in bytes.
ICO_HEADER_SIZE = 6

## Size of one ICONDIRENTRY structure in bytes.
ICO_ENTRY_SIZE = 16

## Smallest edge length stored as PNG. Smaller images are stored as bitmaps,
## which every icon consumer understands. This is also the largest edge length
## an icon can hold; the directory encodes it as 0.
ICO_PNG_MIN_SIZE = 256


def png_chunk(chunk_type, payload):
    """
    @brief Builds a PNG chunk.

    @param chunk_type Four-byte chunk type.
    @param payload    Chunk data.

    @return The chunk including its length and CRC.
    """
    body = chunk_type + payload
    return (struct.pack(">I", len(payload)) + body
            + struct.pack(">I", zlib.crc32(body)))


def encode_png(size, pixels):
    """
    @brief Encodes an image as a PNG stream.

    @param size   Edge length of the image in pixels.
    @param pixels Row-major list of (r, g, b, a) tuples.

    @return The PNG file contents.
    """
    header = struct.pack(">IIBBBBB", size, size, PNG_BIT_DEPTH,
                         PNG_COLOR_TYPE_RGBA, 0, 0, 0)

    # Every scanline is prefixed with its filter type.
    scanlines = bytearray()
    for row in range(size):
        scanlines += PNG_FILTER_NONE
        for pixel in pixels[row * size:(row + 1) * size]:
            scanlines += bytes(pixel)

    return (PNG_SIGNATURE + png_chunk(b"IHDR", header)
            + png_chunk(b"IDAT", zlib.compress(bytes(scanlines)))
            + png_chunk(b"IEND", b""))


def encode_icon_bitmap(size, pixels):
    """
    @brief Encodes an image as the headerless bitmap stored inside icons.

    @param size   Edge length of the image in pixels.
    @param pixels Row-major list of (r, g, b, a) tuples.

    @return The bitmap: a BITMAPINFOHEADER, the BGRA color data and an AND
            mask.
    """
    # The height covers both the color data and the AND mask.
    header = struct.pack("<IiiHHIIiiII", BMP_INFO_HEADER_SIZE, size, size * 2,
                         1, BMP_BITS_PER_PIXEL, 0, 0, 0, 0, 0, 0)

    # Bitmap rows are stored bottom-up, in BGRA order.
    color_data = bytearray()
    for row in reversed(range(size)):
        for red, green, blue, alpha in pixels[row * size:(row + 1) * size]:
            color_data += bytes((blue, green, red, alpha))

    # Transparency comes from the alpha channel, so the mask is left empty.
    mask_row_bits = -(-size // BMP_MASK_ROW_ALIGNMENT) * BMP_MASK_ROW_ALIGNMENT
    mask = bytes(mask_row_bits // 8 * size)
    return header + bytes(color_data) + mask


def encode_icon_image(size, pixels):
    """
    @brief Encodes one image of a Windows icon in the format fitting its size.

    @param size   Edge length of the image in pixels.
    @param pixels Row-major list of (r, g, b, a) tuples.

    @return The encoded image data.
    """
    if size >= ICO_PNG_MIN_SIZE:
        return encode_png(size, pixels)
    return encode_icon_bitmap(size, pixels)


def write_ico(path, images):
    """
    @brief Writes a Windows icon file.

    @param path   Path of the file to write.
    @param images List of (size, pixels) tuples, one per image in the icon.
    """
    encoded = [(size, encode_icon_image(size, pixels))
               for size, pixels in images]

    # Directory entries point at the image data that follows the directory.
    directory = struct.pack("<HHH", 0, ICO_TYPE_ICON, len(encoded))
    offset = ICO_HEADER_SIZE + ICO_ENTRY_SIZE * len(encoded)
    for size, data in encoded:
        directory += struct.pack("<BBBBHHII", size % ICO_PNG_MIN_SIZE,
                                 size % ICO_PNG_MIN_SIZE, 0, 0, 1,
                                 BMP_BITS_PER_PIXEL, len(data), offset)
        offset += len(data)

    with open(path, "wb") as file:
        file.write(directory)
        for _, data in encoded:
            file.write(data)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def generate_netpbm(output_dir, size):
    """
    @brief Renders the icon at one size and writes it in both Netpbm formats.

    @param output_dir Directory the files are written to.
    @param size       Edge length of the image in pixels.
    """
    pixels = render(size)
    base_path = os.path.join(output_dir, f"{ICON_BASE_NAME}_{size}")
    write_pam(base_path + ".pam", size, pixels)
    write_ppm(base_path + ".ppm", size, pixels)
    print(f"Wrote {base_path}.pam and {base_path}.ppm")


def generate_ico(output_dir, sizes):
    """
    @brief Renders the icon at several sizes and writes them as a Windows icon.

    @param output_dir Directory the file is written to.
    @param sizes      Edge lengths, in pixels, of the images in the icon.
    """
    path = os.path.join(output_dir, ICON_BASE_NAME + ".ico")
    write_ico(path, [(size, render(size)) for size in sorted(set(sizes))])
    print(f"Wrote {path}")


def main():
    """
    @brief Parses the command line and generates the requested icons.
    """
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--output-dir", default=DEFAULT_OUTPUT_DIR,
                        help="directory the icons are written to")
    parser.add_argument("--sizes", type=int, nargs="+", default=DEFAULT_SIZES,
                        help="edge lengths, in pixels, of the Netpbm icons")
    parser.add_argument("--ico-sizes", type=int, nargs="+",
                        default=DEFAULT_ICO_SIZES,
                        choices=range(1, ICO_PNG_MIN_SIZE + 1),
                        metavar="N",
                        help="edge lengths, in pixels, of the images inside "
                             "the Windows icon (at most 256)")
    arguments = parser.parse_args()

    os.makedirs(arguments.output_dir, exist_ok=True)
    for size in arguments.sizes:
        generate_netpbm(arguments.output_dir, size)
    generate_ico(arguments.output_dir, arguments.ico_sizes)


if __name__ == "__main__":
    main()
