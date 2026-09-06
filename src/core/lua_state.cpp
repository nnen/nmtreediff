/// \file
/// \brief Implementation of the interpreter wrapper.

#include "core/lua_state.h"

#include <cctype>
#include <cstdint>

#include <sol/sol.hpp>

namespace nmxd {

namespace {

/// \brief How many Lua instructions run between cancellation checks.
///
/// \remarks Small enough that a cancel is acted on inside a millisecond on any
///          machine that can run this tool, large enough that the check costs
///          nothing measurable. The same reasoning as the node batch the parsers
///          check on.
constexpr int kInstructionsBetweenChecks = 10000;

/// \brief Where a stopped script's token lives while a hook can see it.
///
/// \remarks A Lua debug hook is a plain function pointer with no room for a
///          captured value, and the state's extra space is already spoken for
///          by sol2. One token per thread is enough, because a state belongs to
///          one thread by construction.
thread_local const std::stop_token* tCancellation = nullptr;

/// \brief Stops a script when its owner has asked for a cancel.
///
/// \param lua The interpreter running the script.
///
/// \remarks Raises a Lua error, which unwinds as a C++ exception because the
///          library is compiled as C++. sol2 turns that back into a failed
///          result at the call site.
void checkCancellation(lua_State* lua, lua_Debug*) {
    if (tCancellation != nullptr && tCancellation->stop_requested()) {
        luaL_error(lua, "cancelled");
    }
}

}  // namespace

LuaState::LuaState() : state_(std::make_unique<sol::state>()) {
    // The files these scripts come from are the user's own: two under the home
    // directory and one named on the command line. That is the same trust a
    // shell gives a startup file, so the standard libraries are all present. If
    // configuration ever arrives from somewhere the user did not choose, this
    // is the line that has to change.
    state_->open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math,
                           sol::lib::os, sol::lib::io, sol::lib::package);
}

LuaState::~LuaState() { stopWatching(); }

LuaState::LuaState(LuaState&&) noexcept = default;
LuaState& LuaState::operator=(LuaState&&) noexcept = default;

void LuaState::watchForCancellation(std::stop_token token) {
    token_ = std::make_unique<std::stop_token>(std::move(token));
    tCancellation = token_.get();
    lua_sethook(state_->lua_state(), checkCancellation, LUA_MASKCOUNT,
                kInstructionsBetweenChecks);
}

void LuaState::stopWatching() {
    if (state_ != nullptr) {
        lua_sethook(state_->lua_state(), nullptr, 0, 0);
    }
    tCancellation = nullptr;
    token_.reset();
}

std::string trimLuaError(const std::string& message) {
    // Lua prefixes an error with "chunk:line: ". The caller already knows which
    // file it read and reports the line separately, so repeating both would
    // make every message read twice.
    const std::size_t first = message.find(':');
    if (first == std::string::npos) {
        return message;
    }
    const std::size_t second = message.find(':', first + 1);
    if (second == std::string::npos) {
        return message;
    }

    std::size_t at = second + 1;
    while (at < message.size() && std::isspace(static_cast<unsigned char>(message[at])) != 0) {
        ++at;
    }
    return at < message.size() ? message.substr(at) : message;
}

std::uint32_t luaErrorLine(const std::string& message) {
    const std::size_t first = message.find(':');
    if (first == std::string::npos) {
        return 0;
    }

    std::uint32_t line = 0;
    bool any = false;
    for (std::size_t at = first + 1; at < message.size(); ++at) {
        const unsigned char c = static_cast<unsigned char>(message[at]);
        if (std::isdigit(c) == 0) {
            break;
        }
        line = line * 10 + static_cast<std::uint32_t>(c - '0');
        any = true;
    }
    return any ? line : 0;
}

}  // namespace nmxd
