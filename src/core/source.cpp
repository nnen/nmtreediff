/// \file
/// \brief Implementation of file loading and the line index.

#include "core/source.h"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace nmxd {

const char* describe(LoadError error) noexcept {
    switch (error) {
        case LoadError::NotFound:
            return "file not found";
        case LoadError::NotAFile:
            return "path is not a regular file";
        case LoadError::NotReadable:
            return "file could not be read";
        case LoadError::TooLarge:
            return "file is larger than the 2 GB limit";
    }
    return "unknown error";
}

Result<SourceFile, LoadError> SourceFile::load(const std::filesystem::path& path, std::string label) {
    std::error_code ec;

    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status)) {
        return fail(LoadError::NotFound);
    }
    if (!std::filesystem::is_regular_file(status)) {
        return fail(LoadError::NotAFile);
    }

    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return fail(LoadError::NotReadable);
    }
    if (size > kMaxBytes) {
        return fail(LoadError::TooLarge);
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return fail(LoadError::NotReadable);
    }

    SourceFile file;
    file.bytes_.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        stream.read(file.bytes_.data(), static_cast<std::streamsize>(size));
        if (stream.bad()) {
            return fail(LoadError::NotReadable);
        }
        // A text file opened in binary mode can still come up short if it is
        // truncated underneath us; keep only what was actually read.
        file.bytes_.resize(static_cast<std::size_t>(stream.gcount()));
    }

    file.path_ = path;
    file.label_ = label.empty() ? path.string() : std::move(label);
    file.buildLineIndex();
    return file;
}

SourceFile SourceFile::fromMemory(std::string bytes, std::string label,
                                  std::filesystem::path path) {
    SourceFile file;
    file.bytes_ = std::move(bytes);
    file.label_ = std::move(label);
    file.path_ = std::move(path);
    file.buildLineIndex();
    return file;
}

void SourceFile::buildLineIndex() {
    lineStarts_.clear();
    lineStarts_.push_back(0);

    // Only '\n' starts a new line. A CRLF file keeps its '\r' at the end of the
    // line content, which callers trim for display; the byte offsets stay
    // faithful to the file on disk, which is what spans depend on.
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (bytes_[i] == '\n' && i + 1 < bytes_.size()) {
            lineStarts_.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }
    lineStarts_.shrink_to_fit();
}

std::string_view SourceFile::line(std::size_t index) const noexcept {
    if (index >= lineStarts_.size()) {
        return {};
    }
    const std::size_t begin = lineStarts_[index];
    const std::size_t end = (index + 1 < lineStarts_.size()) ? lineStarts_[index + 1] : bytes_.size();

    std::string_view view(bytes_);
    view = view.substr(begin, end - begin);
    if (!view.empty() && view.back() == '\n') {
        view.remove_suffix(1);
    }
    if (!view.empty() && view.back() == '\r') {
        view.remove_suffix(1);
    }
    return view;
}

std::uint32_t SourceFile::lineStart(std::size_t index) const noexcept {
    if (index >= lineStarts_.size()) {
        return static_cast<std::uint32_t>(bytes_.size());
    }
    return lineStarts_[index];
}

std::size_t SourceFile::lineAt(std::uint32_t offset) const noexcept {
    const auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    if (it == lineStarts_.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(lineStarts_.begin(), it) - 1);
}

LineCol SourceFile::locate(std::uint32_t offset) const noexcept {
    const std::size_t index = lineAt(offset);
    const std::uint32_t start = lineStarts_[index];
    return LineCol{static_cast<std::uint32_t>(index + 1), offset >= start ? offset - start + 1 : 1};
}

std::string_view SourceFile::slice(SourceSpan span) const noexcept {
    if (span.empty()) {
        return {};
    }
    const std::size_t begin = std::min<std::size_t>(span.begin, bytes_.size());
    const std::size_t end = std::min<std::size_t>(span.end, bytes_.size());
    return std::string_view(bytes_).substr(begin, end - begin);
}

}  // namespace nmxd
