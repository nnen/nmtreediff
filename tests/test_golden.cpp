// getenv is the portable way to read an environment variable, and the value
// is only compared against a couple of characters.
#define _CRT_SECURE_NO_WARNINGS

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
// Set NMXD_UPDATE_GOLDEN=1 to rewrite the expectations, then read the diff
// before committing it.

namespace fs = std::filesystem;

namespace {

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

std::vector<fs::path> goldenCases() {
    std::vector<fs::path> cases;
    if (!fs::exists(goldenRoot())) {
        return cases;
    }
    for (const auto& entry : fs::directory_iterator(goldenRoot())) {
        if (entry.is_directory() && fs::exists(entry.path() / "left.xml")) {
            cases.push_back(entry.path());
        }
    }
    std::sort(cases.begin(), cases.end());
    return cases;
}

}  // namespace

TEST_CASE("the golden corpus is present", "[golden]") {
    // A silently empty corpus would let every matching regression through.
    CHECK(goldenCases().size() >= 8);
}

TEST_CASE("golden cases match their expectations", "[golden]") {
    const auto registry = nmxd::makeDefaultRegistry();

    for (const auto& directory : goldenCases()) {
        const std::string caseName = directory.filename().string();
        INFO("case " << caseName);

        const auto leftSource =
            nmxd::SourceFile::load(directory / "left.xml", "left");
        const auto rightSource =
            nmxd::SourceFile::load(directory / "right.xml", "right");
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

        const fs::path expectedPath = directory / "expected.txt";
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

    for (const auto& directory : goldenCases()) {
        INFO("case " << directory.filename().string());
        const auto source = nmxd::SourceFile::load(directory / "left.xml", "left");
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
