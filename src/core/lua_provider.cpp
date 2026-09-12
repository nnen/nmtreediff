/// \file
/// \brief Implementation of the scripted format provider.

#include "core/lua_provider.h"

#include <cstdint>
#include <memory>
#include <stdexcept>

#include <sol/sol.hpp>

#include "core/dom.h"
#include "core/lua_shape.h"
#include "core/lua_state.h"
#include "core/registry.h"
#include "core/shape.h"

namespace nmxd {

namespace {

/// \brief Score returned when the file's extension is one the script claimed.
///
/// \remarks The same number generic XML uses for its own extensions. A scripted
///          format that wants to win over the format it is built on says so by
///          claiming the extension, not by outbidding it.
constexpr int kExtensionScore = 90;

/// \brief The function a script writes to say what a document means.
constexpr const char* kShapeFunction = "shape";

/// \brief A format whose shape is decided by a script.
///
/// \remarks A scripted provider shapes a tree; it does not parse bytes. The
///          base format's read() does that, and the script's `shape` function
///          is handed the result as a document, together with the builder and
///          its queue. A script with no `shape` reads exactly like the format
///          it sits on.
class ScriptedProvider final : public IFormatProvider {
public:
    /// \brief Builds a provider from a declaration.
    ///
    /// \param spec What the script asked for.
    /// \param base The provider that does the reading.
    /// \param script The script to reload in each worker.
    ScriptedProvider(ScriptedProviderSpec spec, const IFormatProvider& base, std::string script)
        : spec_(std::move(spec)), base_(base), script_(std::move(script)) {
        for (const std::string& extension : spec_.extensions) {
            extensionViews_.push_back(extension);
        }
        for (const std::string& property : spec_.propertyOrder) {
            propertyViews_.push_back(property);
        }
    }

    std::string_view name() const override { return spec_.name; }
    std::string_view displayName() const override { return spec_.displayName; }

    std::span<const std::string_view> defaultExtensions() const override {
        return extensionViews_;
    }

    int score(const SourceFile& source) const override {
        return claimsExtension(source) ? kExtensionScore : 0;
    }

    GraphDirection graphDirection() const override { return spec_.direction; }

    std::span<const std::string_view> subtitleProperties() const override {
        return base_.subtitleProperties();
    }

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        return rankFromList(propertyViews_, propertyName);
    }

    Result<Tree, ParseError> read(const SourceFile& source, std::stop_token token) const override {
        // The base format reads the bytes; a script never does.
        return base_.read(source, std::move(token));
    }

    void shape(ShapeContext& context) const override {
        // One interpreter per call, because this runs on a worker and a Lua
        // state is not thread safe. Two sides of a diff parse at once. The
        // jobs the script queues are closures inside the interpreter, so it
        // has to outlive the drain, which the context arranges.
        auto state = std::make_shared<LuaState>();
        state->watchForCancellation(context.token());
        bindShapeApi(state->get());
        context.keepAlive(state);

        sol::optional<sol::table> body = loadBody(state->get());
        if (!body) {
            throw std::runtime_error("the script no longer declares this provider");
        }
        const sol::optional<sol::protected_function> shape = (*body)[kShapeFunction];
        if (!shape) {
            // No opinion is the identity: the format reads like its base.
            copyDocument(context);
            return;
        }
        runShapeFunction(state->get(), *shape, context);
    }

private:
    /// \brief Runs the script and finds the table this provider was declared
    ///        with.
    ///
    /// \param lua The interpreter to run in.
    ///
    /// \returns The declaration's body, or nothing when the script failed or
    ///          no longer declares this provider.
    ///
    /// \remarks The other configuration functions are bound to do nothing. The
    ///          script is being reread for its provider declarations, and
    ///          setting the graph direction a second time from a worker thread
    ///          would be a surprise.
    [[nodiscard]] sol::optional<sol::table> loadBody(sol::state& lua) const {
        sol::optional<sol::table> found;

        lua.set_function("formats", [](sol::object) {});
        lua.set_function("fallback", [](sol::object) {});
        lua.set_function("graph_direction", [](sol::object) {});
        lua.set_function("exit_key", [](sol::object) {});
        lua.set_function("provider", [this, &found](std::string declared) {
            return [this, &found, declared](sol::table body) {
                if (declared == spec_.name) {
                    found = body;
                }
            };
        });

        const sol::protected_function_result result =
            lua.safe_script(script_, sol::script_pass_on_error);
        if (!result.valid()) {
            return sol::nullopt;
        }
        return found;
    }

    ScriptedProviderSpec spec_;
    const IFormatProvider& base_;
    std::string script_;
    std::vector<std::string_view> extensionViews_;
    std::vector<std::string_view> propertyViews_;
};

}  // namespace

std::unique_ptr<IFormatProvider> makeScriptedProvider(const ScriptedProviderSpec& spec,
                                                      const IFormatProvider& base,
                                                      std::string script) {
    return std::make_unique<ScriptedProvider>(spec, base, std::move(script));
}

std::vector<std::string> addScriptedProviders(ProviderRegistry& registry,
                                              const ProviderConfig& config) {
    std::vector<std::string> unknown;
    for (const ScriptedProviderSpec& spec : config.providers) {
        const IFormatProvider* base = registry.byName(spec.base);
        if (base == nullptr) {
            unknown.push_back(spec.base);
            continue;
        }
        // Registered before the built-ins already there, so a scripted format
        // that claims an extension wins the tie against the format it is built
        // on. Claiming it is the deliberate act; the tie is not.
        registry.addFirst(makeScriptedProvider(spec, *base, spec.source));
    }
    return unknown;
}

}  // namespace nmxd
