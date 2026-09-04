#pragma once

// SourceFile holds the raw bytes of one side of a diff, plus the line index
// that both views need. Node spans are byte offsets into these bytes, which is
// what lets a click in the node view scroll the text view and back again.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace nmxd {

// A half-open byte range [begin, end) into a SourceFile.
struct SourceSpan {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    [[nodiscard]] bool empty() const noexcept { return end <= begin; }
    [[nodiscard]] std::uint32_t length() const noexcept { return empty() ? 0u : end - begin; }

    friend bool operator==(const SourceSpan&, const SourceSpan&) = default;
};

// One-based, as humans and editors count them.
struct LineCol {
    std::uint32_t line = 1;
    std::uint32_t column = 1;

    friend bool operator==(const LineCol&, const LineCol&) = default;
};

enum class LoadError {
    NotFound,
    NotAFile,
    NotReadable,
    TooLarge,
};

[[nodiscard]] const char* describe(LoadError error) noexcept;

class SourceFile {
public:
    // Byte spans are 32-bit, so this is the hard ceiling on a single side.
    static constexpr std::uint64_t kMaxBytes = 2ull * 1024 * 1024 * 1024 - 1;

    SourceFile() = default;

    // Reads the whole file. Called on a worker, never on the frame loop.
    // The label is what the interface shows; it defaults to the path, and a
    // version control system can override it via the command line.
    [[nodiscard]] static Result<SourceFile, LoadError> load(const std::filesystem::path& path,
                                                            std::string label = {});

    // For tests and for content that never came from disk.
    [[nodiscard]] static SourceFile fromMemory(std::string bytes, std::string label);

    [[nodiscard]] const std::string& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::string_view text() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

    [[nodiscard]] const std::string& label() const noexcept { return label_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    // A file always has at least one line, even when it is empty.
    [[nodiscard]] std::size_t lineCount() const noexcept { return lineStarts_.size(); }

    // Zero-based index; the returned view excludes the line terminator.
    [[nodiscard]] std::string_view line(std::size_t index) const noexcept;

    [[nodiscard]] std::uint32_t lineStart(std::size_t index) const noexcept;

    // Zero-based line containing the given byte offset.
    [[nodiscard]] std::size_t lineAt(std::uint32_t offset) const noexcept;

    // One-based line and column for a byte offset, for display and messages.
    [[nodiscard]] LineCol locate(std::uint32_t offset) const noexcept;

    [[nodiscard]] std::string_view slice(SourceSpan span) const noexcept;

private:
    void buildLineIndex();

    std::string bytes_;
    std::string label_;
    std::filesystem::path path_;
    std::vector<std::uint32_t> lineStarts_{0};
};

}  // namespace nmxd
