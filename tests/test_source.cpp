#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "core/source.h"

using nmtreediff::LineCol;
using nmtreediff::LoadError;
using nmtreediff::SourceFile;
using nmtreediff::SourceSpan;

TEST_CASE("an empty file still has one line", "[source]") {
    const auto file = SourceFile::fromMemory("", "empty");
    CHECK(file.lineCount() == 1);
    CHECK(file.line(0).empty());
    CHECK(file.locate(0) == LineCol{1, 1});
}

TEST_CASE("line index covers every line", "[source]") {
    const auto file = SourceFile::fromMemory("alpha\nbeta\ngamma", "three");
    REQUIRE(file.lineCount() == 3);
    CHECK(file.line(0) == "alpha");
    CHECK(file.line(1) == "beta");
    CHECK(file.line(2) == "gamma");
    CHECK(file.line(3).empty());
}

TEST_CASE("a trailing newline does not add an empty line", "[source]") {
    // A file ending in a newline has as many lines as it has terminators, which
    // is what every editor shows and what a line diff has to agree with.
    const auto file = SourceFile::fromMemory("alpha\nbeta\n", "trailing");
    REQUIRE(file.lineCount() == 2);
    CHECK(file.line(1) == "beta");
}

TEST_CASE("carriage returns are trimmed for display but not for offsets", "[source]") {
    const auto file = SourceFile::fromMemory("alpha\r\nbeta\r\n", "crlf");
    REQUIRE(file.lineCount() == 2);
    CHECK(file.line(0) == "alpha");
    CHECK(file.line(1) == "beta");
    // 'b' sits at offset 7 in the raw bytes, after "alpha\r\n".
    CHECK(file.lineStart(1) == 7);
    CHECK(file.locate(7) == LineCol{2, 1});
}

TEST_CASE("offsets map back to line and column", "[source]") {
    const auto file = SourceFile::fromMemory("alpha\nbeta\ngamma", "three");
    CHECK(file.locate(0) == LineCol{1, 1});
    CHECK(file.locate(4) == LineCol{1, 5});
    CHECK(file.locate(6) == LineCol{2, 1});
    CHECK(file.locate(11) == LineCol{3, 1});
    CHECK(file.lineAt(11) == 2);
}

TEST_CASE("a span slices the original bytes", "[source]") {
    const auto file = SourceFile::fromMemory("<root><child/></root>", "xml");
    CHECK(file.slice(SourceSpan{6, 14}) == "<child/>");
    CHECK(file.slice(SourceSpan{0, 0}).empty());
    // A span running past the end is clamped rather than reading out of bounds.
    CHECK(file.slice(SourceSpan{14, 999}) == "</root>");
}

TEST_CASE("loading reports why it failed", "[source]") {
    const auto missing = SourceFile::load("no/such/file/anywhere.xml");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error() == LoadError::NotFound);

    const auto directory = SourceFile::load(std::filesystem::temp_directory_path());
    REQUIRE_FALSE(directory.ok());
    CHECK(directory.error() == LoadError::NotAFile);
}

TEST_CASE("loading a real file keeps its bytes and labels it", "[source]") {
    const auto path = std::filesystem::temp_directory_path() / "nmtreediff_source_test.xml";
    {
        std::ofstream out(path, std::ios::binary);
        out << "<root>\n  <child id=\"1\"/>\n</root>\n";
    }

    const auto loaded = SourceFile::load(path, "left");
    REQUIRE(loaded.ok());
    const SourceFile& file = loaded.value();
    CHECK(file.label() == "left");
    CHECK(file.lineCount() == 3);
    CHECK(file.line(1) == "  <child id=\"1\"/>");

    const auto unlabelled = SourceFile::load(path);
    REQUIRE(unlabelled.ok());
    CHECK(unlabelled.value().label() == path.string());

    std::filesystem::remove(path);
}
