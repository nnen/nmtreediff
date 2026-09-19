#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/result.h"

using nmtreediff::fail;
using nmtreediff::Result;

namespace {

enum class Error { Bad, Worse };

}  // namespace

TEST_CASE("a successful result carries its value", "[result]") {
    Result<int, Error> result = 42;
    REQUIRE(result.ok());
    CHECK(static_cast<bool>(result));
    CHECK(result.value() == 42);
}

TEST_CASE("a failed result carries its error", "[result]") {
    Result<int, Error> result = fail(Error::Worse);
    REQUIRE_FALSE(result.ok());
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(result.error() == Error::Worse);
    CHECK(result.valueOr(7) == 7);
}

TEST_CASE("a move-only value survives being moved out", "[result]") {
    Result<std::string, Error> result = std::string("a long string that will not fit in place");
    REQUIRE(result.ok());
    const std::string taken = std::move(result).value();
    CHECK(taken.size() > 16);
}

TEST_CASE("a void result reports success and failure", "[result]") {
    Result<void, Error> good;
    CHECK(good.ok());

    Result<void, Error> bad = fail(Error::Bad);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error() == Error::Bad);
}

TEST_CASE("value and error may be the same type", "[result]") {
    // The variant is indexed rather than matched by type, so this stays
    // unambiguous.
    Result<std::string, std::string> ok = std::string("value");
    Result<std::string, std::string> bad = fail(std::string("error"));
    CHECK(ok.ok());
    CHECK(ok.value() == "value");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error() == "error");
}
