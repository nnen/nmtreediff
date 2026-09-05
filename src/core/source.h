#pragma once

/// \file
/// \brief The raw bytes of one side of a diff, with the line index both views
///        depend on.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace nmxd {

/// \brief A half-open byte range `[begin, end)` into a SourceFile.
///
/// \remarks Node spans are expressed in these coordinates. They are what lets a
///          click in the node view scroll the text view, and a click in the
///          text view select a node.
struct SourceSpan {
    /// \brief Offset of the first byte in the range.
    std::uint32_t begin = 0;
    /// \brief Offset one past the last byte in the range.
    std::uint32_t end = 0;

    /// \brief Reports whether the range covers no bytes.
    ///
    /// \returns `true` when the range is empty or inverted.
    [[nodiscard]] bool empty() const noexcept { return end <= begin; }

    /// \brief Returns the number of bytes covered.
    ///
    /// \returns The byte count, or zero when the range is empty.
    [[nodiscard]] std::uint32_t length() const noexcept { return empty() ? 0u : end - begin; }

    /// \brief Compares two spans for equality.
    friend bool operator==(const SourceSpan&, const SourceSpan&) = default;
};

/// \brief A one-based line and column position, as humans and editors count
///        them.
struct LineCol {
    /// \brief One-based line number.
    std::uint32_t line = 1;
    /// \brief One-based column number, counted in bytes.
    std::uint32_t column = 1;

    /// \brief Compares two positions for equality.
    friend bool operator==(const LineCol&, const LineCol&) = default;
};

/// \brief Why a file could not be read.
enum class LoadError {
    NotFound,     ///< No file exists at the given path.
    NotAFile,     ///< The path names a directory or other non-file.
    NotReadable,  ///< The file exists but could not be opened or read.
    TooLarge,     ///< The file exceeds SourceFile::kMaxBytes.
};

/// \brief Converts a load error into a phrase suitable for a message.
///
/// \param error The error to describe.
///
/// \returns A short lower-case phrase, never null.
[[nodiscard]] const char* describe(LoadError error) noexcept;

/// \brief One side of a diff: its bytes, its label, and its line index.
///
/// \remarks Instances are immutable once loaded, which is what makes them safe
///          to share between a worker and the frame loop through a snapshot.
class SourceFile {
public:
    /// \brief The largest file that can be loaded.
    ///
    /// \remarks Byte spans are 32-bit, so this is a hard ceiling rather than a
    ///          tuning knob.
    static constexpr std::uint64_t kMaxBytes = 2ull * 1024 * 1024 * 1024 - 1;

    /// \brief Constructs an empty file with no bytes and no path.
    SourceFile() = default;

    /// \brief Reads a whole file from disk and builds its line index.
    ///
    /// \param path The file to read.
    /// \param label Title to show for this side. Defaults to the path when
    ///        empty, and a version control system can override it.
    ///
    /// \returns The loaded file, or a LoadError explaining the failure.
    ///
    /// \remarks Called on a worker thread, never on the frame loop.
    [[nodiscard]] static Result<SourceFile, LoadError> load(const std::filesystem::path& path,
                                                            std::string label = {});

    /// \brief Builds a file from bytes already in memory.
    ///
    /// \param bytes The content.
    /// \param label Title to show for this side.
    ///
    /// \returns The constructed file, with its line index already built.
    ///
    /// \remarks For tests, and for content that never came from disk.
    [[nodiscard]] static SourceFile fromMemory(std::string bytes, std::string label);

    /// \brief Returns the file content.
    ///
    /// \returns The bytes exactly as read, with no newline translation.
    [[nodiscard]] const std::string& bytes() const noexcept { return bytes_; }

    /// \brief Returns the file content as a view.
    ///
    /// \returns A view over the bytes, valid for the lifetime of this object.
    [[nodiscard]] std::string_view text() const noexcept { return bytes_; }

    /// \brief Returns the size of the file.
    ///
    /// \returns The number of bytes.
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    /// \brief Reports whether the file has any content.
    ///
    /// \returns `true` when the file holds no bytes.
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

    /// \brief Returns the title shown for this side.
    ///
    /// \returns The label, which is the path when none was supplied.
    [[nodiscard]] const std::string& label() const noexcept { return label_; }

    /// \brief Returns the path this file was read from.
    ///
    /// \returns The path, empty for content built with fromMemory().
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// \brief Returns the number of lines.
    ///
    /// \returns At least one: an empty file holds a single empty line, the same
    ///          way every editor counts it.
    [[nodiscard]] std::size_t lineCount() const noexcept { return lineStarts_.size(); }

    /// \brief Returns the content of one line.
    ///
    /// \param index Zero-based line index.
    ///
    /// \returns The line without its terminator, or an empty view when \p index
    ///          is past the end.
    ///
    /// \remarks A trailing carriage return is trimmed for display, but the byte
    ///          offsets reported elsewhere stay faithful to the file on disk.
    [[nodiscard]] std::string_view line(std::size_t index) const noexcept;

    /// \brief Returns the offset at which a line begins.
    ///
    /// \param index Zero-based line index.
    ///
    /// \returns The byte offset, or the file size when \p index is past the end.
    [[nodiscard]] std::uint32_t lineStart(std::size_t index) const noexcept;

    /// \brief Finds the line containing a byte offset.
    ///
    /// \param offset Byte offset into the file.
    ///
    /// \returns The zero-based index of the line containing \p offset.
    [[nodiscard]] std::size_t lineAt(std::uint32_t offset) const noexcept;

    /// \brief Converts a byte offset into a line and column.
    ///
    /// \param offset Byte offset into the file.
    ///
    /// \returns The one-based position, for display and for messages.
    [[nodiscard]] LineCol locate(std::uint32_t offset) const noexcept;

    /// \brief Returns the bytes covered by a span.
    ///
    /// \param span The range to slice.
    ///
    /// \returns A view over the bytes, clamped to the file rather than reading
    ///          out of bounds.
    [[nodiscard]] std::string_view slice(SourceSpan span) const noexcept;

private:
    void buildLineIndex();

    std::string bytes_;
    std::string label_;
    std::filesystem::path path_;
    std::vector<std::uint32_t> lineStarts_{0};
};

}  // namespace nmxd
