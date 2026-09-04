#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "core/provider.h"
#include "core/session.h"

using nmxd::Session;
using nmxd::SessionRequest;
using nmxd::Stage;

namespace {

std::filesystem::path writeTemp(const std::string& name, const std::string& contents) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return path;
}

}  // namespace

TEST_CASE("a new session starts idle with no sources", "[session]") {
    Session session(1);
    const auto snapshot = session.snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->stage == Stage::Idle);
    CHECK_FALSE(snapshot->hasSources());
}

TEST_CASE("opening a pair publishes both sources", "[session]") {
    const auto left = writeTemp("nmxd_session_left.xml", "<root>\n  <a/>\n</root>\n");
    const auto right = writeTemp("nmxd_session_right.xml", "<root>\n  <b/>\n</root>\n");

    Session session(2);
    session.open(SessionRequest{left, right, "left", "right"});
    session.waitIdle();

    const auto snapshot = session.snapshot();
    REQUIRE(snapshot);
    // The last stage the pipeline reaches today. Matching lands at M3.
    REQUIRE(snapshot->stage == Stage::TreesParsed);
    REQUIRE(snapshot->hasSources());
    CHECK(snapshot->left->label() == "left");
    CHECK(snapshot->right->label() == "right");
    CHECK(snapshot->left->lineCount() == 3);
    CHECK(snapshot->elapsedMillis >= 0.0);

    REQUIRE(snapshot->text);
    CHECK(snapshot->text->modifiedRows == 1);
    CHECK(snapshot->text->equalRows == 2);

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("the sources are published before the diff finishes", "[session]") {
    const auto left = writeTemp("nmxd_session_staged_left.xml", "<root>\n  <a/>\n</root>\n");
    const auto right = writeTemp("nmxd_session_staged_right.xml", "<root>\n  <b/>\n</root>\n");

    Session session(2);
    const auto before = session.snapshotVersion();
    session.open(SessionRequest{left, right, "left", "right"});
    session.waitIdle();

    // Loading, then the sources, then the diff: three publications, which is
    // what puts the raw text on screen while the alignment is still running.
    CHECK(session.snapshotVersion() - before >= 3);

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("a missing file fails with the path in the message", "[session]") {
    const auto left = writeTemp("nmxd_session_present.xml", "<root/>\n");

    Session session(1);
    session.open(SessionRequest{left, "no/such/file.xml", "left", "right"});
    session.waitIdle();

    const auto snapshot = session.snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->stage == Stage::Failed);
    CHECK(snapshot->message.find("no/such/file.xml") != std::string::npos);
    CHECK(snapshot->message.find("not found") != std::string::npos);

    std::filesystem::remove(left);
}

TEST_CASE("each publication bumps the snapshot version", "[session]") {
    const auto left = writeTemp("nmxd_session_v_left.xml", "<a/>\n");
    const auto right = writeTemp("nmxd_session_v_right.xml", "<b/>\n");

    Session session(1);
    const auto before = session.snapshotVersion();

    session.open(SessionRequest{left, right, "left", "right"});
    session.waitIdle();

    // The version is what lets the frame loop tell a new snapshot from the one
    // it drew last frame without comparing contents.
    CHECK(session.snapshotVersion() > before);

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("both sides are parsed into trees", "[session]") {
    const auto left = writeTemp("nmxd_session_tree_left.xml", "<root><a id=\"1\"/></root>\n");
    const auto right = writeTemp("nmxd_session_tree_right.xml", "<root><a id=\"1\"/><b/></root>\n");

    Session session(2);
    session.open(SessionRequest{left, right, "left", "right", ""});
    session.waitIdle();

    const auto snapshot = session.snapshot();
    REQUIRE(snapshot);
    REQUIRE(snapshot->stage == Stage::TreesParsed);
    REQUIRE(snapshot->leftTree);
    REQUIRE(snapshot->rightTree);
    REQUIRE(snapshot->provider != nullptr);

    CHECK(snapshot->provider->name() == "xml");
    CHECK(snapshot->leftTree->size() == 2);
    CHECK(snapshot->rightTree->size() == 3);

    // The text diff is still there: parsing adds to the snapshot rather than
    // replacing what an earlier stage produced.
    REQUIRE(snapshot->text);
    CHECK_FALSE(snapshot->text->identical());

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("an unknown format is reported, not silently sniffed", "[session]") {
    const auto left = writeTemp("nmxd_session_fmt_left.xml", "<root/>\n");
    const auto right = writeTemp("nmxd_session_fmt_right.xml", "<root/>\n");

    Session session(1);
    session.open(SessionRequest{left, right, "left", "right", "not-a-format"});
    session.waitIdle();

    const auto snapshot = session.snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->stage == Stage::Failed);
    CHECK(snapshot->message.find("not-a-format") != std::string::npos);
    CHECK(snapshot->message.find("xml") != std::string::npos);

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}

TEST_CASE("a malformed document fails after the text diff succeeded", "[session]") {
    // The text view still works on a file the parser rejects, which is exactly
    // why the stages publish separately.
    const auto left = writeTemp("nmxd_session_bad_left.xml", "<root></root>\n");
    const auto right = writeTemp("nmxd_session_bad_right.xml", "<root><unclosed></root>\n");

    Session session(1);
    session.open(SessionRequest{left, right, "left", "right", ""});
    session.waitIdle();

    const auto snapshot = session.snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->stage == Stage::Failed);
    CHECK(snapshot->message.find("well formed") != std::string::npos);
    REQUIRE(snapshot->text);
    CHECK(snapshot->text->rows.size() > 0);

    std::filesystem::remove(left);
    std::filesystem::remove(right);
}
