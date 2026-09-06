#pragma once

/// \file
/// \brief One Lua interpreter, and the rules everything in it runs under.

#include <memory>
#include <stop_token>
#include <string>

namespace sol {
class state;
}  // namespace sol

namespace nmxd {

/// \brief Owns one Lua interpreter.
///
/// \remarks A Lua state is not thread safe and the provider interface says
///          providers must be callable from several threads at once, because
///          the two sides of a diff parse in parallel. So a state is never
///          shared: each worker that needs one makes its own and loads the same
///          scripts into it. The states share nothing, which also means a
///          script cannot accumulate state between files and quietly make one
///          comparison depend on what was opened before it.
///
///          Created only when a script is actually found. A run with no
///          configuration never builds an interpreter, which is what keeps this
///          off the startup budget.
class LuaState {
public:
    /// \brief Builds an interpreter with the standard libraries loaded.
    LuaState();

    /// \brief Closes the interpreter.
    ~LuaState();

    LuaState(const LuaState&) = delete;
    LuaState& operator=(const LuaState&) = delete;
    /// \brief Takes over another interpreter.
    ///
    /// \param other The interpreter to take over, left holding nothing.
    LuaState(LuaState&& other) noexcept;

    /// \brief Takes over another interpreter, closing this one.
    ///
    /// \param other The interpreter to take over.
    ///
    /// \returns This interpreter.
    LuaState& operator=(LuaState&& other) noexcept;

    /// \brief Returns the underlying interpreter.
    ///
    /// \returns The sol2 state, which is never null.
    [[nodiscard]] sol::state& get() { return *state_; }

    /// \brief Bounds how long a script may run before it is stopped.
    ///
    /// \param token Checked as the script runs; the script is stopped when a
    ///        stop is requested.
    ///
    /// \remarks A script with a loop in it would otherwise hold a worker past a
    ///          cancel. This is not about the script being untrusted, which it
    ///          is not: the files it comes from are the user's own. It is that
    ///          the frame loop does not make exceptions for code the user wrote.
    ///
    ///          The token must outlive the call that runs the script.
    void watchForCancellation(std::stop_token token);

    /// \brief Stops watching, leaving the script free to run to completion.
    void stopWatching();

private:
    std::unique_ptr<sol::state> state_;
    std::unique_ptr<std::stop_token> token_;
};

/// \brief Trims a Lua error down to what is worth showing.
///
/// \param message The message Lua produced.
///
/// \returns The same message without the chunk name and line, which the caller
///          reports separately.
[[nodiscard]] std::string trimLuaError(const std::string& message);

/// \brief Reads the line number out of a Lua error message.
///
/// \param message The message Lua produced.
///
/// \returns The one-based line, or zero when the message does not carry one.
[[nodiscard]] std::uint32_t luaErrorLine(const std::string& message);

}  // namespace nmxd
