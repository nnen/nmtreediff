// getenv is the portable way to read an environment variable, and the value
// is only compared against a couple of characters.
#define _CRT_SECURE_NO_WARNINGS

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/diff.h"
#include "core/registry.h"
#include "core/source.h"

// The golden corpus is the definition of good matching. A change in matching
// quality shows up here as a diff of expected output, which is reviewable in a
// way that a pass or fail is not.
//
// A case is a directory holding a left file, a right file and the expected
// change list. The extension decides the format, so a case in a new format is
// two files and an expectation with nothing else to edit.
//
// Set NMXD_UPDATE_GOLDEN=1 to rewrite the expectations, then read the diff
// before committing it.

namespace fs = std::filesystem;

namespace {

/// \brief One case in the corpus: two files and the format they are in.
struct GoldenCase {
    fs::path directory;
    fs::path left;
    fs::path right;
    std::string extension;
};

fs::path goldenRoot() { return fs::path(NMXD_TESTDATA_DIR) / "golden"; }

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeFile(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

bool updatingGolden() {
    const char* flag = std::getenv("NMXD_UPDATE_GOLDEN");
    return flag != nullptr && *flag != '\0' && *flag != '0';
}

// The extension is not fixed, because the corpus covers every built-in format.
// A directory holding a left file and a matching right file is a case;
// anything else is ignored rather than failing, so that a stray directory does
// not read as a broken test.
std::vector<GoldenCase> goldenCases() {
    std::vector<GoldenCase> cases;
    if (!fs::exists(goldenRoot())) {
        return cases;
    }

    for (const auto& entry : fs::directory_iterator(goldenRoot())) {
        if (!entry.is_directory()) {
            continue;
        }
        for (const auto& file : fs::directory_iterator(entry.path())) {
            if (file.path().stem() != "left") {
                continue;
            }
            const fs::path right = entry.path() / ("right" + file.path().extension().string());
            if (fs::exists(right)) {
                cases.push_back(GoldenCase{entry.path(), file.path(), right,
                                           file.path().extension().string()});
            }
            break;
        }
    }

    std::sort(cases.begin(), cases.end(),
              [](const GoldenCase& a, const GoldenCase& b) { return a.directory < b.directory; });
    return cases;
}

}  // namespace

TEST_CASE("the golden corpus is present", "[golden]") {
    const auto cases = goldenCases();

    // A silently empty corpus would let every matching regression through.
    CHECK(cases.size() >= 8);

    // Both built-in formats are represented, because a corpus that only covers
    // XML would not notice a JSON provider that stopped working.
    std::set<std::string> extensions;
    for (const auto& item : cases) {
        extensions.insert(item.extension);
    }
    CHECK(extensions.count(".xml") == 1);
    CHECK(extensions.count(".json") == 1);
}

TEST_CASE("golden cases match their expectations", "[golden]") {
    const auto registry = nmxd::makeDefaultRegistry();

    for (const auto& item : goldenCases()) {
        const std::string caseName = item.directory.filename().string();
        INFO("case " << caseName);

        const auto leftSource = nmxd::SourceFile::load(item.left, "left");
        const auto rightSource = nmxd::SourceFile::load(item.right, "right");
        REQUIRE(leftSource.ok());
        REQUIRE(rightSource.ok());

        const auto* provider = registry.resolve(leftSource.value());
        REQUIRE(provider != nullptr);

        auto leftTree = provider->parse(leftSource.value(), {});
        auto rightTree = provider->parse(rightSource.value(), {});
        REQUIRE(leftTree.ok());
        REQUIRE(rightTree.ok());

        const auto model = nmxd::diffTrees(leftTree.value(), rightTree.value(), *provider);
        const std::string actual =
            nmxd::serializeChanges(leftTree.value(), rightTree.value(), model);

        const fs::path expectedPath = item.directory / "expected.txt";
        if (updatingGolden()) {
            writeFile(expectedPath, actual);
            continue;
        }

        REQUIRE(fs::exists(expectedPath));
        std::string expected = readFile(expectedPath);
        // Tolerate a checkout that converted line endings.
        expected.erase(std::remove(expected.begin(), expected.end(), '\r'), expected.end());

        INFO("expected:\n" << expected << "\nactual:\n" << actual);
        CHECK(actual == expected);
    }
}

TEST_CASE("diffing a document against itself finds nothing", "[golden]") {
    // Not in the corpus because it has to hold for every case in it.
    const auto registry = nmxd::makeDefaultRegistry();

    for (const auto& item : goldenCases()) {
        INFO("case " << item.directory.filename().string());
        const auto source = nmxd::SourceFile::load(item.left, "left");
        REQUIRE(source.ok());

        const auto* provider = registry.resolve(source.value());
        REQUIRE(provider != nullptr);
        auto tree = provider->parse(source.value(), {});
        REQUIRE(tree.ok());

        const auto model = nmxd::diffTrees(tree.value(), tree.value(), *provider);
        CHECK(model.identical());
        CHECK(model.changes.empty());
    }
}
