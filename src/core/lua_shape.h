#pragma once

/// \file
/// \brief The shaping surface as a script sees it: the document, the builder
///        and the queue, bound into a Lua interpreter.

#include <sol/forward.hpp>

namespace nmxd {

class ShapeContext;

/// \brief Binds the document, the builder handle and the queue into an
///        interpreter.
///
/// \param lua The interpreter to bind into.
///
/// \remarks Registers the types a `shape` function is handed and everything
///          reachable from them. Called once per interpreter, before the
///          script's shape function runs.
void bindShapeApi(sol::state& lua);

/// \brief Runs a script's shape function over a document.
///
/// \param lua The interpreter, with the API bound and the script loaded.
/// \param shape The script's `shape` function.
/// \param context The pass to shape within.
///
/// \throws std::runtime_error with the script's message when the function
///         raises, so the drain records it as a failure.
///
/// \remarks The function is called with the document and the builder. What
///          it queues through the builder's `next` and `later` is run by the
///          drain after this returns, each job catching its own error.
void runShapeFunction(sol::state& lua, sol::protected_function shape, ShapeContext& context);

}  // namespace nmxd
