/// \file
/// \brief Implementation of the scripted format provider.

#include "core/lua_provider.h"

#include <algorithm>
#include <cstdint>

#include <sol/sol.hpp>

#include "core/hash.h"
#include "core/lua_state.h"
#include "core/registry.h"

namespace nmxd {

namespace {

/// \brief How many nodes are shaped between cancellation checks.
constexpr std::size_t kCancelCheckInterval = 1024;

/// \brief Score returned when the file's extension is one the script claimed.
///
/// \remarks The same number generic XML uses for its own extensions. A scripted
///          format that wants to win over the format it is built on says so by
///          claiming the extension, not by outbidding it.
constexpr int kExtensionScore = 90;

/// \brief The function a script writes to say what counts as a node.
constexpr const char* kIsNode = "is_node";
/// \brief The function that says an element describes the node above it.
constexpr const char* kFoldIntoParent = "fold_into_parent";
/// \brief The function that names what sort of node this is.
constexpr const char* kKind = "kind";
/// \brief The function that says what makes a node the same node.
constexpr const char* kIdentity = "identity";
/// \brief The function that titles a node's card.
constexpr const char* kTitle = "title";

/// \brief The word a script returns to mean an identity may travel.
constexpr const char* kStrongWord = "strong";

/// \brief Turns a node into the table a script sees.
///
/// \param tree The tree being shaped.
/// \param id The node to describe.
/// \param lua The interpreter to build the table in.
///
/// \returns A table holding the node's name and its properties by name.
///
/// \remarks Built once per node while the document is open, which is the whole
///          cost model: a script is asked a question once per node, never once
///          per node per frame.
[[nodiscard]] sol::table describeNode(const Tree& tree, NodeId id, sol::state& lua) {
    const Node& node = tree.node(id);
    sol::table element = lua.create_table();
    element["name"] = node.kind;

    sol::table attributes = lua.create_table();
    for (const Property& property : node.properties) {
        attributes[property.name] = property.value;
    }
    element["attr"] = attributes;
    return element;
}

/// \brief A format whose shape is decided by a script.
class ScriptedProvider final : public IFormatProvider {
public:
    /// \brief Builds a provider from a declaration.
    ///
    /// \param spec What the script asked for.
    /// \param base The provider that does the parsing.
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

    int propertyRank(const Tree& tree, NodeId id, std::string_view propertyName) const override {
        (void)tree;
        (void)id;
        return rankFromList(propertyViews_, propertyName);
    }

    Result<Tree, ParseError> parse(const SourceFile& source,
                                   std::stop_token token) const override {
        // The base format reads the bytes. Everything below is shaping, which
        // is the only part the script has an opinion about.
        auto parsed = base_.parse(source, token);
        if (!parsed.ok()) {
            return fail(parsed.error());
        }
        const Tree generic = std::move(parsed).value();
        if (generic.empty()) {
            return fail(ParseError::Empty);
        }

        // One interpreter per call, because this runs on a worker and a Lua
        // state is not thread safe. Two sides of a diff parse at once.
        LuaState state;
        state.watchForCancellation(token);
        sol::optional<sol::table> shape = loadShape(state.get());
        if (!shape) {
            return fail(ParseError::NotWellFormed);
        }

        Shaper shaper(generic, state.get(), *shape, *this);
        return shaper.run(token);
    }

    IdentityKey identity(const Tree& tree, NodeId id) const override {
        // Answered while parsing, because asking a script per node during
        // matching would cross into an interpreter on the hot path.
        const NodeAnnotation& annotation = tree.annotation(id);
        if (annotation.identity.empty()) {
            return IdentityKey{};
        }
        return IdentityKey{annotation.strongIdentity, annotation.identity};
    }

    NodeStyle style(const Tree& tree, NodeId id) const override {
        const Node& node = tree.node(id);
        const NodeAnnotation& annotation = tree.annotation(id);

        NodeStyle style;
        style.title = annotation.title.empty() ? node.kind : annotation.title;
        style.subtitle = annotation.subtitle;

        // Derived from the kind rather than asked for, so two nodes of one kind
        // always agree and a script never has to think about colour.
        const std::uint64_t h = hashBytes(node.kind);
        style.accent = Color{static_cast<std::uint8_t>(110 + (h & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 8) & 0x3F)),
                             static_cast<std::uint8_t>(110 + ((h >> 16) & 0x3F)), 255};
        return style;
    }

private:
    /// \brief Runs the script and finds the table this provider was declared
    ///        with.
    ///
    /// \param lua The interpreter to run in.
    ///
    /// \returns The table of shaping functions, or nothing when the script
    ///          failed or no longer declares this provider.
    ///
    /// \remarks The other configuration functions are bound to do nothing. The
    ///          script is being reread for its provider declarations, and
    ///          setting the graph direction a second time from a worker thread
    ///          would be a surprise.
    [[nodiscard]] sol::optional<sol::table> loadShape(sol::state& lua) const {
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

    /// \brief Turns one generic tree into the tree the script describes.
    class Shaper {
    public:
        /// \brief Prepares a shaping pass.
        ///
        /// \param generic The tree the base provider produced.
        /// \param lua The interpreter the script is loaded in.
        /// \param shape The script's table of functions.
        /// \param provider The provider being run, for its name.
        Shaper(const Tree& generic, sol::state& lua, const sol::table& shape,
               const IFormatProvider& provider)
            : generic_(generic), lua_(lua), shape_(shape), provider_(provider) {}

        /// \brief Runs the pass.
        ///
        /// \param token Checked on a bounded interval.
        ///
        /// \returns The shaped tree, or a ParseError.
        [[nodiscard]] Result<Tree, ParseError> run(const std::stop_token& token) {
            shaped_.setFormatName(std::string(provider_.name()));

            // The document's own root is always a node. A script decides what
            // is inside a document, not whether there is one.
            const NodeId root = generic_.root();
            const NodeId placed = addNode(root, kInvalidNode);
            collectChildren(root, placed, token);
            if (cancelled_) {
                return fail(ParseError::Cancelled);
            }

            shaped_.finalize();
            computeHashes(shaped_, provider_, token);
            if (token.stop_requested()) {
                return fail(ParseError::Cancelled);
            }
            return std::move(shaped_);
        }

    private:
        /// \brief Asks the script a yes-or-no question about a node.
        ///
        /// \param function The name of the function to call.
        /// \param id The node to ask about.
        /// \param whenAbsent What to answer when the script did not supply one.
        ///
        /// \returns The script's answer, or \p whenAbsent.
        [[nodiscard]] bool ask(const char* function, NodeId id, bool whenAbsent) {
            const sol::optional<sol::protected_function> callable = shape_[function];
            if (!callable) {
                return whenAbsent;
            }
            const sol::protected_function_result result = (*callable)(describeNode(generic_, id, lua_));
            if (!result.valid() || result.get_type() == sol::type::nil) {
                return whenAbsent;
            }
            return result.get<bool>();
        }

        /// \brief Adds one generic node to the shaped tree.
        ///
        /// \param id The generic node.
        /// \param parent The shaped parent, or kInvalidNode for the root.
        ///
        /// \returns The shaped node's id.
        NodeId addNode(NodeId id, NodeId parent) {
            const Node& source = generic_.node(id);

            std::string kind = source.kind;
            const sol::optional<sol::protected_function> kindOf = shape_[kKind];
            if (kindOf) {
                const sol::protected_function_result result =
                    (*kindOf)(describeNode(generic_, id, lua_));
                if (result.valid() && result.get_type() == sol::type::string) {
                    kind = result.get<std::string>();
                }
            }

            const NodeId placed = shaped_.add(parent, kind, source.span);
            for (const Property& property : source.properties) {
                shaped_.addProperty(placed, property.name, property.value, property.span);
            }
            annotate(id, placed);
            return placed;
        }

        /// \brief Records what the script says about one node.
        ///
        /// \param id The generic node.
        /// \param placed The shaped node it became.
        void annotate(NodeId id, NodeId placed) {
            NodeAnnotation annotation;

            const sol::optional<sol::protected_function> identityOf = shape_[kIdentity];
            if (identityOf) {
                const sol::protected_function_result result =
                    (*identityOf)(describeNode(generic_, id, lua_));
                if (result.valid() && result.get_type() == sol::type::string) {
                    annotation.identity = result.get<std::string>(0);
                    // A second return value naming strength, so the common case
                    // stays one line and the strong case is deliberate.
                    if (result.return_count() > 1 && result.get_type(1) == sol::type::string) {
                        annotation.strongIdentity = result.get<std::string>(1) == kStrongWord;
                    }
                }
            }

            const sol::optional<sol::protected_function> titleOf = shape_[kTitle];
            if (titleOf) {
                const sol::protected_function_result result =
                    (*titleOf)(describeNode(generic_, id, lua_));
                if (result.valid() && result.get_type() == sol::type::string) {
                    annotation.title = result.get<std::string>(0);
                    if (result.return_count() > 1 && result.get_type(1) == sol::type::string) {
                        annotation.subtitle = result.get<std::string>(1);
                    }
                }
            }

            shaped_.annotate(placed, std::move(annotation));
        }

        /// \brief Folds one generic node's properties into a shaped node.
        ///
        /// \param id The generic node being folded away.
        /// \param owner The shaped node that takes its properties.
        ///
        /// \remarks A folded element usually describes its parent rather than
        ///          standing on its own: a name and a value pair in the file
        ///          become one property of the node above.
        ///
        ///          Anything else keeps its own shape. An element with several
        ///          attributes becomes one property named after the element,
        ///          holding a part per attribute, which is what R7.7 asked for:
        ///          a transform stays one thing with parts rather than becoming
        ///          a handful of loose names.
        void fold(NodeId id, NodeId owner) {
            const Node& source = generic_.node(id);

            const Property* named = source.findProperty("name");
            const Property* valued = source.findProperty("value");
            if (named != nullptr && valued != nullptr) {
                shaped_.addProperty(owner, named->value, valued->value, source.span);
                return;
            }

            Property folded;
            folded.name = source.kind;
            folded.span = source.span;
            if (source.properties.size() == 1) {
                // One attribute and nothing else is a value, not a record.
                folded.value = source.properties.front().value;
            } else {
                folded.children = source.properties;
            }
            shaped_.addProperty(owner, std::move(folded));
        }

        /// \brief Walks a generic node's children into the shaped tree.
        ///
        /// \param id The generic node whose children to walk.
        /// \param owner The shaped node they belong under.
        /// \param token Checked on a bounded interval.
        void collectChildren(NodeId id, NodeId owner, const std::stop_token& token) {
            for (const NodeId child : generic_.node(id).children) {
                if (cancelled_) {
                    return;
                }
                if (++seen_ % kCancelCheckInterval == 0 && token.stop_requested()) {
                    cancelled_ = true;
                    return;
                }
                collectOne(child, owner, token);
            }
        }

        /// \brief Decides what becomes of one generic node.
        ///
        /// \param id The generic node.
        /// \param owner The shaped node it sits under.
        /// \param token Checked on a bounded interval.
        ///
        /// \remarks Two answers, not three. An element is a node, or it is a
        ///          property of the node above it. There used to be a third,
        ///          walking through an element and keeping nothing of it, and
        ///          that is what made it possible to lose content by accident.
        ///          A diff tool that silently drops what it does not recognise
        ///          is the one thing a reviewer cannot forgive, so it is gone.
        ///
        ///          An element that is not a node but contains nodes keeps
        ///          both: its own name and attributes become a property, and
        ///          the nodes inside it attach to the nearest ancestor node.
        ///          Swallowing them into property content would be simpler and
        ///          would lose them, which is the problem this rule exists to
        ///          remove.
        void collectOne(NodeId id, NodeId owner, const std::stop_token& token) {
            if (ask(kIsNode, id, true)) {
                const NodeId placed = addNode(id, owner);
                collectChildren(id, placed, token);
                return;
            }

            fold(id, owner);
            collectChildren(id, owner, token);
        }

        const Tree& generic_;
        sol::state& lua_;
        const sol::table& shape_;
        const IFormatProvider& provider_;
        Tree shaped_;
        std::size_t seen_ = 0;
        bool cancelled_ = false;
    };

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
