/// \file
/// \brief Implementation of the scripted format provider.

#include "core/lua_provider.h"

#include <algorithm>
#include <any>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "core/hash.h"
#include "core/lua_state.h"
#include "core/registry.h"
#include "core/shape.h"
#include "core/tree_shape.h"
#include "formats/json_shape.h"
#include "formats/xml_shape.h"

namespace nmxd {

namespace {

/// \brief Score returned when the file's extension is one the script claimed.
///
/// \remarks The same number generic XML uses for its own extensions. A scripted
///          format that wants to win over the format it is built on says so by
///          claiming the extension, not by outbidding it.
constexpr int kExtensionScore = 90;

/// \brief The function a script writes to be told an element has started.
constexpr const char* kEnter = "enter";
/// \brief The function a script writes to be told an element has ended.
constexpr const char* kExit = "exit";

/// \brief The word a script passes to mean an identity may travel.
constexpr const char* kStrongWord = "strong";

/// \brief The usertype a script sees an element as.
///
/// \remarks The usertype names are prefixed so that a script cannot mistake
///          them for something it declared, and so that an error message
///          names what it is about.
constexpr const char* kElementType = "nmxd.Element";
/// \brief The usertype a script sees an element's items as.
constexpr const char* kItemsType = "nmxd.Items";
/// \brief The usertype a script sees the builder as.
constexpr const char* kBuilderType = "nmxd.Builder";
/// \brief The usertype a script sees an emitted node as.
constexpr const char* kNodeType = "nmxd.Node";
/// \brief The usertype a script sees an emitted property as.
constexpr const char* kPropertyType = "nmxd.Property";
/// \brief The usertype a script sees the enter control as.
constexpr const char* kFrameType = "nmxd.Frame";

// ------------------------------------------------------------------ handles

/// \brief What every handle a script holds points back to.
///
/// \remarks One per parse. A handle carries a pointer to this and enough to
///          check that what it names is still there, because a script may
///          keep a handle longer than the callback that made it, and touching
///          a closed element has to be an error rather than a crash.
struct ScriptContext {
    sol::state* lua = nullptr;         ///< The interpreter.
    ShapeSession* session = nullptr;   ///< The session driving the shaper.
    Builder* builder = nullptr;        ///< The builder of the exit in progress.
    EnterControl* control = nullptr;   ///< The control of the enter in progress.
    std::uint64_t epoch = 0;           ///< Counts callbacks, to date handles.
};

/// \brief A script's view of one open element.
struct ElementHandle {
    ScriptContext* context = nullptr;  ///< What the handle points back to.
    std::uint32_t depth = 0;           ///< The element's depth.
    std::uint64_t serial = 0;          ///< The element's serial.

    /// \brief Resolves the element.
    ///
    /// \returns The element.
    ///
    /// \remarks Throws when the element is no longer open, which sol2 turns
    ///          into a Lua error at the point of use.
    [[nodiscard]] Element& element() const {
        Element* found = context->session->frameAt(depth, serial);
        if (found == nullptr) {
            throw std::runtime_error("this element is no longer open");
        }
        return *found;
    }
};

/// \brief Makes a handle for an element.
///
/// \param context The context the handle belongs to.
/// \param element The element.
///
/// \returns The handle.
[[nodiscard]] ElementHandle handleFor(ScriptContext& context, const Element& element) {
    return ElementHandle{&context, element.depth(), element.serial()};
}

/// \brief A script's view of the items made inside one element.
struct ItemsHandle {
    ElementHandle owner;  ///< The element the items belong to.

    /// \brief Resolves the items.
    ///
    /// \returns The owning element's items.
    [[nodiscard]] Items& items() const { return owner.element().items(); }
};

/// \brief A script's view of the builder handed to exit.
struct BuilderHandle {
    ScriptContext* context = nullptr;  ///< What the handle points back to.
    std::uint64_t epoch = 0;           ///< The callback the handle was made in.

    /// \brief Resolves the builder.
    ///
    /// \returns The builder.
    ///
    /// \remarks Throws once the exit it was made for has returned.
    [[nodiscard]] Builder& builder() const {
        if (context->builder == nullptr || context->epoch != epoch) {
            throw std::runtime_error("the builder belongs to an exit that has finished");
        }
        return *context->builder;
    }
};

/// \brief A script's handle on a node it emitted.
struct NodeHandle {
    ScriptContext* context = nullptr;  ///< What the handle points back to.
    std::uint64_t epoch = 0;           ///< The callback the handle was made in.
    NodeBuilder node;                  ///< The builder for the node.

    /// \brief Resolves the node builder.
    ///
    /// \returns The builder.
    ///
    /// \remarks Throws once the exit it was made in has returned.
    [[nodiscard]] NodeBuilder builder() const {
        if (context->epoch != epoch) {
            throw std::runtime_error("the node belongs to an exit that has finished");
        }
        return node;
    }
};

/// \brief A script's handle on a property it emitted.
struct PropertyHandle {
    ScriptContext* context = nullptr;  ///< What the handle points back to.
    std::uint64_t epoch = 0;           ///< The callback the handle was made in.
    PropertyBuilder property;          ///< The builder for the property.

    /// \brief Resolves the property builder.
    ///
    /// \returns The builder.
    ///
    /// \remarks Throws once the exit it was made in has returned.
    [[nodiscard]] PropertyBuilder builder() const {
        if (context->epoch != epoch) {
            throw std::runtime_error("the property belongs to an exit that has finished");
        }
        return property;
    }
};

/// \brief A script's handle on the control handed to enter.
struct FrameHandle {
    ElementHandle owner;      ///< The element being entered.
    std::uint64_t epoch = 0;  ///< The callback the handle was made in.

    /// \brief Resolves the control.
    ///
    /// \returns The control.
    ///
    /// \remarks Throws once the enter it was made for has returned.
    [[nodiscard]] EnterControl& control() const {
        if (owner.context->control == nullptr || owner.context->epoch != epoch) {
            throw std::runtime_error("the frame belongs to an enter that has finished");
        }
        return *owner.context->control;
    }
};

/// \brief Reads an element handle out of an argument that may be nil.
///
/// \param argument What the script passed.
///
/// \returns The element, or `nullptr` for nil or anything else.
[[nodiscard]] Element* elementArgument(const sol::object& argument) {
    if (!argument.is<ElementHandle>()) {
        return nullptr;
    }
    return &argument.as<ElementHandle>().element();
}

/// \brief Reads an items handle out of an argument.
///
/// \param argument What the script passed.
///
/// \returns The items.
///
/// \remarks Throws when the argument is not an items handle, so a script
///          that passes the wrong thing hears about it.
[[nodiscard]] Items& itemsArgument(const sol::object& argument) {
    if (!argument.is<ItemsHandle>()) {
        throw std::runtime_error("expected the items of an element");
    }
    return argument.as<ItemsHandle>().items();
}

/// \brief Reads a span out of an argument that may name an element.
///
/// \param argument What the script passed.
///
/// \returns The element's span, or an empty span for nil or anything else.
[[nodiscard]] SourceSpan spanArgument(const sol::object& argument) {
    const Element* from = elementArgument(argument);
    return from == nullptr ? SourceSpan{} : from->span();
}

/// \brief Reads a list of attribute names to leave out.
///
/// \param names What the script passed after the element.
///
/// \returns The names, as strings a call can point at.
[[nodiscard]] std::vector<std::string> namesArgument(const sol::variadic_args& names) {
    std::vector<std::string> collected;
    for (const auto& name : names) {
        if (name.is<std::string>()) {
            collected.push_back(name.as<std::string>());
        }
    }
    return collected;
}

/// \brief Hands over an element's attributes with some left out.
///
/// \param source The element whose attributes to take.
/// \param skip The names to leave out, as a script gave them.
/// \param add Called with each attribute that is kept.
template <class Add>
void attributesExcept(const Element& source, const std::vector<std::string>& skip, Add&& add) {
    for (const Property& attribute : source.attributes()) {
        if (std::find(skip.begin(), skip.end(), attribute.name) == skip.end()) {
            add(attribute);
        }
    }
}

/// \brief Returns the value a script stored on an element.
///
/// \param element The element.
/// \param lua The interpreter, for nil.
///
/// \returns The value, or nil when nothing was stored.
[[nodiscard]] sol::object dataOf(const Element& element, sol::state& lua) {
    const sol::object* stored = std::any_cast<sol::object>(&element.data());
    return stored == nullptr ? sol::make_object(lua, sol::lua_nil) : *stored;
}

/// \brief Builds, once, the table of an element's attributes.
///
/// \param element The element.
/// \param lua The interpreter to build it in.
///
/// \returns The table, kept on the element so that a script reading it
///          twice pays once.
[[nodiscard]] sol::table attributeTable(Element& element, sol::state& lua) {
    if (const sol::table* cached = std::any_cast<sol::table>(&element.cache())) {
        return *cached;
    }
    sol::table table = lua.create_table();
    for (const Property& attribute : element.attributes()) {
        table[attribute.name] = attribute.value;
    }
    element.cache() = table;
    return table;
}

/// \brief Installs the usertypes the new form uses into an interpreter.
///
/// \param lua The interpreter.
///
/// \remarks Every entry resolves its handle on each use, so a stale handle
///          raises a Lua error naming the problem rather than reading memory
///          that is no longer there.
void installHandles(sol::state& lua) {
    lua.new_usertype<ElementHandle>(
        kElementType, sol::no_constructor,
        "name", sol::property([](const ElementHandle& h) { return std::string(h.element().name()); }),
        "text", sol::property([](const ElementHandle& h) { return std::string(h.element().text()); }),
        "depth", sol::property([](const ElementHandle& h) { return h.element().depth(); }),
        "index", sol::property([](const ElementHandle& h) { return h.element().index(); }),
        "child_count", sol::property([](const ElementHandle& h) { return h.element().childCount(); }),
        "closed", sol::property([](const ElementHandle& h) { return h.element().closed(); }),
        "span", sol::property([](const ElementHandle& h) {
            const SourceSpan span = h.element().span();
            sol::table table = h.context->lua->create_table();
            table["start"] = span.begin;
            table["stop"] = span.end;
            return table;
        }),
        "attr", sol::property([](const ElementHandle& h) {
            return attributeTable(h.element(), *h.context->lua);
        }),
        "parent", sol::property([](const ElementHandle& h) -> sol::object {
            const Element* parent = h.element().parent();
            if (parent == nullptr) {
                return sol::make_object(*h.context->lua, sol::lua_nil);
            }
            return sol::make_object(*h.context->lua, handleFor(*h.context, *parent));
        }),
        "items", sol::property([](const ElementHandle& h) { return ItemsHandle{h}; }),
        "data", sol::property(
            [](const ElementHandle& h) { return dataOf(h.element(), *h.context->lua); },
            [](const ElementHandle& h, sol::object value) { h.element().data() = value; }),
        "ancestor", [](const ElementHandle& h, const std::string& name) -> sol::object {
            const Element* found = h.element().ancestor(name);
            if (found == nullptr) {
                return sol::make_object(*h.context->lua, sol::lua_nil);
            }
            return sol::make_object(*h.context->lua, handleFor(*h.context, *found));
        });

    lua.new_usertype<ItemsHandle>(
        kItemsType, sol::no_constructor,
        sol::meta_function::length, [](const ItemsHandle& h) { return h.items().size(); });

    lua.new_usertype<NodeHandle>(
        kNodeType, sol::no_constructor,
        "attributes", [](NodeHandle& h, const ElementHandle& source, sol::variadic_args skip) {
            NodeBuilder node = h.builder();
            attributesExcept(source.element(), namesArgument(skip),
                             [&node](const Property& attribute) { node.property(attribute); });
            return h;
        },
        "text", [](NodeHandle& h, const ElementHandle& source) {
            h.builder().text(source.element());
            return h;
        },
        "property", [](NodeHandle& h, std::string name, std::string value, sol::object source) {
            h.builder().property(std::move(name), std::move(value), spanArgument(source));
            return h;
        },
        "adopt", [](NodeHandle& h, sol::object items) {
            h.builder().adopt(std::move(itemsArgument(items)));
            return h;
        },
        "kind", [](NodeHandle& h, std::string kind) {
            h.builder().kind(std::move(kind));
            return h;
        },
        "identity", [](NodeHandle& h, sol::object value, sol::optional<std::string> strength) {
            if (value.is<std::string>()) {
                h.builder().identity(value.as<std::string>(), strength && *strength == kStrongWord);
            }
            return h;
        },
        "title", [](NodeHandle& h, sol::object title, sol::optional<std::string> subtitle) {
            if (title.is<std::string>()) {
                h.builder().title(title.as<std::string>(), subtitle.value_or(std::string{}));
            }
            return h;
        },
        "ordered", [](NodeHandle& h, bool ordered) {
            h.builder().orderedChildren(ordered);
            return h;
        });

    lua.new_usertype<PropertyHandle>(
        kPropertyType, sol::no_constructor,
        "value", [](PropertyHandle& h, std::string value) {
            h.builder().value(std::move(value));
            return h;
        },
        "part", [](PropertyHandle& h, std::string name, sol::object value, sol::object source) {
            h.builder().part(std::move(name),
                             value.is<std::string>() ? value.as<std::string>() : std::string{},
                             spanArgument(source));
            return h;
        },
        "attributes", [](PropertyHandle& h, const ElementHandle& source, sol::variadic_args skip) {
            PropertyBuilder property = h.builder();
            attributesExcept(source.element(), namesArgument(skip),
                             [&property](const Property& attribute) { property.part(attribute); });
            return h;
        },
        "adopt", [](PropertyHandle& h, sol::object items) {
            h.builder().adopt(std::move(itemsArgument(items)));
            return h;
        },
        "ordered", [](PropertyHandle& h, bool ordered) {
            h.builder().ordered(ordered);
            return h;
        },
        "collapse", [](PropertyHandle& h) {
            h.builder().collapse();
            return h;
        });

    lua.new_usertype<BuilderHandle>(
        kBuilderType, sol::no_constructor,
        "node", [](const BuilderHandle& h, std::string kind, sol::object source) {
            NodeBuilder node = h.builder().node(std::move(kind), spanArgument(source));
            return NodeHandle{h.context, h.epoch, node};
        },
        "property", [](const BuilderHandle& h, std::string name, sol::object value,
                       sol::object source) {
            PropertyBuilder property = h.builder().property(
                std::move(name), value.is<std::string>() ? value.as<std::string>() : std::string{},
                spanArgument(source));
            return PropertyHandle{h.context, h.epoch, property};
        },
        "forward", [](const BuilderHandle& h, sol::object items) {
            h.builder().forward(std::move(itemsArgument(items)));
        },
        "default", [](const BuilderHandle& h, const ElementHandle& element) {
            h.builder().defaultTreatment(element.element());
        },
        "drop", [](const BuilderHandle& h) { h.builder().drop(); });

    lua.new_usertype<FrameHandle>(
        kFrameType, sol::no_constructor,
        "default", [](const FrameHandle& h) { h.control().useDefault(); },
        "opaque", [](const FrameHandle& h) { h.control().opaque(); },
        "data", sol::property(
            [](const FrameHandle& h) {
                return dataOf(h.owner.element(), *h.owner.context->lua);
            },
            [](const FrameHandle& h, sol::object value) { h.owner.element().data() = value; }));
}

/// \brief The enter-and-exit form, written as a shaper.
///
/// \remarks A script's `enter` is told an element has started and may leave
///          a value for its descendants or take the subtree away. Its `exit`
///          is told the element has ended, with the items made inside it,
///          and says what it becomes through the builder. Either may be
///          absent: no `enter` steers nothing, and no `exit` on an element
///          gives it the default treatment.
///
///          An error raised inside either is read as no answer. Whatever
///          `exit` emitted before the error stays, and what it did not adopt
///          goes up as it always does.
class ScriptShaper final : public IShaper {
public:
    /// \brief Prepares to shape with a script's table of functions.
    ///
    /// \param lua The interpreter the script is loaded in.
    /// \param shape The script's table.
    ScriptShaper(sol::state& lua, sol::table shape) : shape_(std::move(shape)) {
        context_.lua = &lua;
        enter_ = shape_[kEnter];
        exit_ = shape_[kExit];
    }

    void attach(ShapeSession& session) override { context_.session = &session; }

    void enter(Element& element, EnterControl& control) override {
        if (!enter_) {
            return;
        }
        ++context_.epoch;
        context_.control = &control;
        const ElementHandle handle = handleFor(context_, element);
        (void)(*enter_)(handle, FrameHandle{handle, context_.epoch});
        context_.control = nullptr;
    }

    void exit(Element& element, Builder& out) override {
        if (!exit_) {
            return;
        }
        ++context_.epoch;
        context_.builder = &out;
        (void)(*exit_)(handleFor(context_, element), BuilderHandle{&context_, context_.epoch});
        context_.builder = nullptr;
    }

private:
    ScriptContext context_;
    sol::table shape_;
    sol::optional<sol::protected_function> enter_;
    sol::optional<sol::protected_function> exit_;
};

// ------------------------------------------------------------ the provider

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
        // One interpreter per call, because this runs on a worker and a Lua
        // state is not thread safe. Two sides of a diff parse at once.
        LuaState state;
        state.watchForCancellation(token);
        installHandles(state.get());
        sol::optional<sol::table> shape = loadShape(state.get());
        if (!shape) {
            return fail(ParseError::NotWellFormed);
        }

        ScriptShaper shaper(state.get(), *shape);

        // XML and JSON have walkers of their own, so the script sees the
        // elements as the parser meets them. Any other base reads the file
        // first, and the script sees that reading's tree.
        if (base_.name() == "xml") {
            return shapeXmlDocument(source, shaper, *this, token);
        }
        if (base_.name() == "json") {
            return shapeJsonDocument(source, shaper, *this, token);
        }
        auto parsed = base_.parse(source, token);
        if (!parsed.ok()) {
            return fail(parsed.error());
        }
        return shapeTree(parsed.value(), source, shaper, *this, token);
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

    bool childrenOrdered(const Tree& tree, NodeId id) const override {
        return !tree.annotation(id).childrenUnordered;
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
